#include "servfetch.h"

#include <arpa/inet.h>
#include <cJSON.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "serv.h"

struct response {
    char* memory;
    size_t size;
};

static size_t mem_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    struct response* mem = (struct response*)userp;

    char* ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Failed to reallocate: %s\n", strerror(errno));
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

static const size_t string_offsets[3] = {
    offsetof(struct servinfo, hn_off),
    offsetof(struct servinfo, gm_off),
    offsetof(struct servinfo, ln_off),
};

#define BUF_SIZE 4096
#define GETOBJ(cjson, name) cJSON_GetObjectItemCaseSensitive(cjson, name)
static int parse_servers(const char* json, struct json_keys keys,
                         struct servlist** out) {
    int ret = -1;
    if (json == NULL) return -1;

    cJSON* parsed = cJSON_Parse(json);
    if (parsed == NULL) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "Json parsing error: %s\n", error_ptr);
        }
        ret = -2;
        goto end;
    }

    if (!cJSON_IsArray(parsed)) {
        fprintf(stderr, "Server list isn't an array\n");
        ret = -2;
        goto end_free;
    }
    size_t arr_size = cJSON_GetArraySize(parsed);
    if (out == NULL) {
        ret = arr_size > INT_MAX ? INT_MAX : arr_size;
        goto end;
    }

    size_t alloc_size =
        sizeof(struct servlist) + sizeof(struct servinfo) * arr_size;
    struct servlist* list = malloc(alloc_size);
    if (list == NULL) {
        fprintf(stderr, "Malloc of %zu bytes failed: %s\n", alloc_size,
                strerror(errno));
        goto end_free;
    }
    list->cap = arr_size;

    size_t server_count = 0;
    char* textbuf = malloc(BUF_SIZE);
    if (textbuf == NULL) {
        fprintf(stderr, "Failed to allocate textbuf\n");
        goto end_freetext;
    }

    size_t buf_used = 0;
    size_t buf_size = BUF_SIZE;
    const cJSON *serv = NULL, *pa, *pc, *pm, *ip, *txt[3];
    struct servinfo* info;
    ret = -2;
    cJSON_ArrayForEach(serv, parsed) {
        info = list->servs + server_count;

        // clang-format off
        if (!cJSON_IsObject(serv)
            || !cJSON_IsBool(pa = GETOBJ(serv, keys.pa_key))
            || !cJSON_IsString(ip = GETOBJ(serv, keys.ip_key))
            || !cJSON_IsNumber(pc = GETOBJ(serv, keys.pc_key))
            || !cJSON_IsNumber(pm = GETOBJ(serv, keys.pm_key))
            || !cJSON_IsString(txt[0] = GETOBJ(serv, keys.string_keys[0]))
            || !cJSON_IsString(txt[1] = GETOBJ(serv, keys.string_keys[1]))
            || !cJSON_IsString(txt[2] = GETOBJ(serv, keys.string_keys[2]))
            )
            goto end_freetext;
        // clang-format on

        info->pa = cJSON_IsTrue(pa);

        if (pc->valueint < 0 || pc->valueint > 1000) goto end_freetext;
        info->pc = pc->valueint;

        if (pm->valueint < 0 || pm->valueint > 1000) goto end_freetext;
        info->pm = pm->valueint;

        char ip_str[INET_ADDRSTRLEN + 6];
        strncpy(ip_str, ip->valuestring, sizeof(ip_str) - 1);
        ip_str[sizeof(ip_str) - 1] = '\0';

        char* port = strchr(ip_str, ':');
        if (port == NULL) goto end_freetext;
        *port = '\0';
        port++;

        if (inet_pton(AF_INET, ip_str, &info->ip) != 1) goto end_freetext;

        long portx = strtol(port, NULL, 10);
        if (portx <= 0 || portx > UINT16_MAX) goto end_freetext;
        info->port = htons(portx);

        size_t offset[3];
        for (size_t i = 0; i < 3; i++) {
            offset[i] = buf_used;
            size_t len = strlen(txt[i]->valuestring);
            if (SIZE_MAX - buf_used <= len) {
                ret = -1;
                goto end_freetext;
            }

            buf_used += len + 1;
        }

        if (buf_used > buf_size) {
            while (buf_used > buf_size) buf_size += buf_size / 2;
            char* new_buf = realloc(textbuf, buf_size);
            if (new_buf == NULL) {
                ret = -1;
                goto end_freetext;
            }
            textbuf = new_buf;
        }

        for (size_t i = 0; i < 3; i++) {
            strcpy(textbuf + offset[i], txt[i]->valuestring);
            *(size_t*)(((char*)info) + string_offsets[i]) = offset[i];
        }
        info->txt = NULL;

        server_count++;
    }

    list->len = server_count;
    list->num_displayed = server_count;
    list->txt = textbuf;

    ret = 0;
    *out = list;

end_freetext:
    if (ret < 0) {
        free(textbuf);
        free(list);
    }
end_free:
    cJSON_Delete(parsed);
end:
    return ret;
}

static char* get_resp(const char* url) {
    struct response json = {0};
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "curl init failed\n");
        return NULL;
    }

    CURLcode resp;

    resp = curl_easy_setopt(curl, CURLOPT_URL, url);

    resp = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_cb);
    resp = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json);

    resp = curl_easy_perform(curl);
    if (resp != CURLE_OK) {
        fprintf(stderr, "Failed to retrieve %s: %s\n", url,
                curl_easy_strerror(resp));
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_easy_cleanup(curl);

    return json.memory;
}

struct servlist* fetch_servers(const char* url, struct json_keys keys) {
    struct servlist* list;
    char* raw = get_resp(url);
    if (raw == NULL) return NULL;

    if (parse_servers(raw, keys, &list) < 0) {
        free(raw);
        return NULL;
    }

    free(raw);
    return list;
}

int parse_masterlist(const char* txt, struct masterstate** out) {
    if (txt == NULL) return -1;
    char buf[INET_ADDRSTRLEN + 6];

    struct masterstate* state = NULL;
    size_t nmemb = 0;
    size_t cap = 0;

    struct in_addr addr;
    uint16_t port;

    const char* cursor = txt;
    size_t len;
    for (;;) {
        /* checking for [0-9]|.|: would probably be redundant, as it gets
         * validated right after. */
        len = strcspn(cursor, "\n");
        if (len >= sizeof(buf) - 1) goto loopend; /* skip invalid entries */
        buf[len] = '\0';

        memcpy(buf, cursor, len);
        char* colon = strchr(buf, ':');
        if (colon == NULL) goto loopend;
        *colon = '\0';
        colon++;

        if (inet_pton(AF_INET, buf, &addr) != 1) goto loopend;
        long portx = atol(colon);
        if (portx <= 0 || portx > UINT16_MAX) goto loopend;
        port = htons((uint16_t)portx);

        struct sockaddr_in sock_addr = {
            .sin_family = AF_INET,
            .sin_addr = addr,
            .sin_port = port,
        };

        if (nmemb >= cap) {
            while (nmemb >= cap) {
                if (SIZE_MAX - cap < cap / 2) {
                    cap = nmemb;
                    goto end;
                }
                cap = (cap == 0) ? 4 : cap + cap / 2;
            }
            struct masterstate* tmp_buf =
                realloc(state, sizeof(struct masterstate)
                                   + sizeof(struct sockaddr_in) * cap);
            if (tmp_buf == NULL) {
                cap = nmemb;
                goto end;
            }

            state = tmp_buf;
        }

        state->entries[nmemb] = sock_addr;

        nmemb++;
    loopend:
        cursor += len;
        if (*cursor == '\0') break;
        cursor++;
    }
end:
    if (state != NULL) {
        state->len = nmemb;
        state->refs = 0;
        state->buf = (struct servquery_textbuf){0};
        state->next_entry = 0;
        state->userdata = NULL;
        *out = state;
        return 0;
    }
    return -1;
}

struct masterstate* fetch_masterstate(const char* url) {
    struct masterstate* state;
    char* raw = get_resp(url);
    if (raw == NULL) return NULL;

    if (parse_masterlist(raw, &state) < 0) state = NULL;

    free(raw);
    return state;
}

static int sockfd = -1;
int servquery_init(void) {
    if (sockfd >= 0) return 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -errno;

    struct timeval tout = {
        .tv_sec = 1,
    };

    if ((setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tout, sizeof(tout)) < 0)
        || (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tout, sizeof(tout)) < 0)) {
        close(fd);
        return -errno;
    }

    sockfd = fd;
    return 0;
}

int servquery_getfd(void) {
    if (sockfd < 0) return -EBADFD;

    return sockfd;
}

#define DGRAM_MAX 65535
#define QUERY_LEN 11
static int servquery_constructreq(const struct sockaddr_in serv, char opcode,
                                  unsigned char req[QUERY_LEN]) {
    switch (opcode) {
        case SERVQUERY_OPCODE_INFO:
        case SERVQUERY_OPCODE_CLIENTS:
        case SERVQUERY_OPCODE_RULES:
            break;
        default:
            return -EINVAL;
    }

    /* https://open.mp/docs/tutorials/QueryMechanism#serialized-data */
    memcpy(req, "SAMP", 4);
    memcpy(req + 4, &serv.sin_addr, 4);
    memcpy(req + 8, &serv.sin_port, 2);
    /* both the commented out version and the one above seemingly WORK(???) The
     * wiki says you should do the one on the bottom but it's bogus because it
     * results in different packets on different architectures... I'm going to
     * ASSUME that you're expected to put the port in network order, because
     * that _makes sense_ */
    /*
    req[8] = serv.sin_port & 0xFF;
    req[9] = serv.sin_port >> 8;
    */

    req[10] = opcode;
    return 0;
}

static int resp_inspect(struct sockaddr_in* out_from,
                        unsigned char resp[DGRAM_MAX]) {
    struct sockaddr_in sender;
    socklen_t sendersize = sizeof(sender);

    size_t exp = QUERY_LEN;

    ssize_t ret;
    if ((ret = recvfrom(sockfd, resp, DGRAM_MAX, MSG_PEEK,
                        (struct sockaddr*)&sender, &sendersize))
        < 0)
        return -errno;
    if ((size_t)ret < exp)
        return -EBADE;
    else if (sender.sin_family != AF_INET) {
        return -EBADE;
    }

    memcpy(out_from, &sender, sizeof(struct sockaddr_in));

    int op = resp[10];

    unsigned char req[QUERY_LEN];
    int err = servquery_constructreq(sender, op, req);
    if (err < 0) return -EBADE;

    if (memcmp(req, resp, exp) != 0) return -EBADE;

    return op;
}

int servquery_inspectresp(struct sockaddr_in* out_from) {
    unsigned char resp[DGRAM_MAX];
    return resp_inspect(out_from, resp);
}

/* doesn't null out out_sender if fails before receiving. only works if socket
 * isn't shared with another thread */
static ssize_t test_consume(const struct sockaddr_in* exp_sender,
                            struct sockaddr_in* out_sender,
                            enum servquery_opcode exp_opcode,
                            unsigned char resp[DGRAM_MAX]) {
    struct sockaddr_in sender;

    int op = resp_inspect(&sender, resp);
    if (op < 0) return op;
    if (out_sender != NULL) *out_sender = sender;

    if (op != (int)exp_opcode && exp_opcode != SERVQUERY_OPCODE_UNKNOWN)
        return -EBADE;

    if (exp_sender != NULL
        && memcmp(exp_sender, &sender, sizeof(struct sockaddr_in)) != 0)
        return -EBADE;

    ssize_t ret;
    if ((ret = recvfrom(sockfd, resp, DGRAM_MAX, 0, NULL, 0)) < 0)
        return -errno;

    return ret;
}

int servquery_discardresp(void) {
    unsigned char resp[1];
    if ((recvfrom(sockfd, resp, 1, 0, NULL, NULL)) < 0) return -errno;

    return 0;
}

int servquery_sendreq(const struct sockaddr_in serv, char opcode) {
    if (sockfd < 0) return -EBADFD;

    unsigned char req[QUERY_LEN];
    int ret = servquery_constructreq(serv, opcode, req);
    if (ret < 0) return ret;

    if (sendto(sockfd, req, QUERY_LEN, 0, (struct sockaddr*)&serv, sizeof(serv))
        < 0)
        return -errno;

    return 0;
}

bool servquery_respavail(void) {
    if (sockfd < 0) return false;
    struct pollfd fd = {
        .fd = sockfd,
        .events = POLLIN,
    };

    int nfd;
    while ((nfd = poll(&fd, 1, 0)) < 0) {
        if (errno == EAGAIN || errno == EINTR) continue;
        return false;
    }

    if (fd.revents & POLLIN) return true;
    return false;
}

static int parse_info(const unsigned char resp[DGRAM_MAX], const size_t rlen,
                      size_t rexp, struct servinfo* out,
                      struct servquery_textbuf* buf);

static int parse_rules(const unsigned char resp[DGRAM_MAX], const size_t rlen,
                       size_t rexp, struct servrules** out,
                       struct servquery_textbuf* buf);

static int parse_clients(const unsigned char resp[DGRAM_MAX], const size_t rlen,
                         size_t rexp, struct servclients** out,
                         struct servquery_textbuf* buf);

int servquery_getresp(union servquery_out* out, const struct sockaddr_in* from,
                      struct sockaddr_in* out_from,
                      enum servquery_opcode exp_op,
                      struct servquery_textbuf* buf, bool* out_consumed) {
    if (out_consumed != NULL) *out_consumed = false;
    if (out_from != NULL) memset(out_from, 0, sizeof(struct sockaddr_in));

    if (out == NULL) return -EINVAL;
    unsigned char resp[DGRAM_MAX];

    size_t exp = QUERY_LEN;
    ssize_t ret = test_consume(from, out_from, exp_op, resp);
    if (ret < 0) return ret;
    if (out_consumed != NULL) *out_consumed = true;
    int op = resp[10];

    /* https://open.mp/docs/tutorials/QueryMechanism#response */
    switch (op) {
        case SERVQUERY_OPCODE_INFO:
            exp += 17; /* all non-variable-length fields sum up to 17. */
            break;
        case SERVQUERY_OPCODE_RULES:
        case SERVQUERY_OPCODE_CLIENTS:
            exp += 2;
            break;
        default:
            abort(); /* should be unreachable atp */
    }

    if ((size_t)ret < exp) return -EBADE;

    union servquery_out lout = *out;
    switch (op) {
        case SERVQUERY_OPCODE_INFO:
            ret = parse_info(resp, (size_t)ret, exp, &lout.info, buf);
            memcpy(&lout.info.ip, resp + 4, 4);
            memcpy(&lout.info.port, resp + 8, 2);
            break;
        case SERVQUERY_OPCODE_RULES:
            ret = parse_rules(resp, (size_t)ret, exp, &lout.rules, buf);
            break;
        case SERVQUERY_OPCODE_CLIENTS:
            ret = parse_clients(resp, (size_t)ret, exp, &lout.clients, buf);
            break;
        default:
            abort();
    }

    if (ret < 0) return ret;
    *out = lout;
    return op;
}

int servquery_getanyresp(union servquery_out* out, struct sockaddr_in* out_from,
                         struct servquery_textbuf* buf, bool* out_consumed) {
    return servquery_getresp(out, NULL, out_from, SERVQUERY_OPCODE_UNKNOWN, buf,
                             out_consumed);
}

static int try_grow_textbuf(struct servquery_textbuf* buf,
                            size_t needed_space) {
    if (buf == NULL) return -EINVAL;

    size_t bsize = buf->size;
    size_t bleft = bsize - buf->off;
    char* textbuf = buf->txt;
    if (bleft < needed_space) {
        while (bleft < needed_space) {
            if ((SIZE_MAX - bsize) < bsize / 2) return -EOVERFLOW;
            bsize = (bsize == 0) ? needed_space : bsize + bsize / 2;
            bleft = bsize - buf->off;
        }
        textbuf = realloc(textbuf, bsize);
    }
    if (textbuf == NULL) return -errno;

    buf->txt = textbuf;
    buf->size = bsize;

    return 0;
}

static int parse_info(const unsigned char resp[DGRAM_MAX], const size_t rlen,
                      size_t rexp, struct servinfo* out,
                      struct servquery_textbuf* buf) {
    if (out == NULL) return -EINVAL;
    if (buf != NULL) {
        if (buf->size < buf->off) return -EINVAL;
        if (buf->txt == NULL && buf->size > 0) return -EINVAL;
    }

    const unsigned char* curs = resp + QUERY_LEN;
    struct servinfo info = *out;
    memcpy(&info.pa, curs, 1);
    curs++;

    memcpy(&info.pc, curs, 2);
    curs += 2;

    memcpy(&info.pm, curs, 2);
    curs += 2;

    /* leave other fields untouched */
    if (buf == NULL) goto end;

    /* calculate buffer size */

    size_t offsets[3];
    uint32_t lengths[3];
    size_t buf_occupied = 0;
    /* use another variable instead of then backtracking curs */
    const unsigned char* curs_tmp = curs;
    for (size_t i = 0; i < 3; i++) {
        memcpy(lengths + i, curs_tmp, 4);
        if (lengths[i] > SIZE_MAX - rexp) return -EOVERFLOW;

        rexp += lengths[i];
        if (rlen < rexp) return -EBADE;

        offsets[i] = buf_occupied;
        buf_occupied += lengths[i] + 1;
        curs_tmp += 4 + lengths[i];
    }

    int ret = try_grow_textbuf(buf, buf_occupied);
    if (ret < 0) return ret;

    for (size_t i = 0; i < 3; i++) {
        curs += 4;
        memcpy(buf->txt + buf->off + offsets[i], curs, lengths[i]);
        buf->txt[buf->off + offsets[i] + lengths[i]] = '\0';
        curs += lengths[i];
    }

    info.hn_off = buf->off + offsets[0];
    info.gm_off = buf->off + offsets[1];
    info.ln_off = buf->off + offsets[2];

    buf->off += buf_occupied;

    /* don't set info.txt, let the caller decide what to do with it */

end:

    *out = info;
    return 0;
}

/* IGNORES buf, parameter is kept solely for similar signature */
static int parse_rules(const unsigned char resp[DGRAM_MAX], const size_t rlen,
                       size_t rexp, struct servrules** out,
                       struct servquery_textbuf* buf) {
    if (out == NULL) return -EINVAL;
    /* this function completely ignores buf, as in my honest opinion it
     * currently makes NO SENSE for it to use a user provided buf. */
    (void)buf;

    const unsigned char* cursor = resp + QUERY_LEN;
    uint16_t rc;
    memcpy(&rc, cursor, 2);
    cursor += 2;

    /* check to make sure that we are not overflowing exp by adding rc * 2 to
     * it. (length of a rule and a value is described with a single byte) */
    if ((SIZE_MAX - rexp) / 2 < (size_t)rc) return -EOVERFLOW;
    rexp += rc * 2;

    if (rlen < rexp) return -EBADE;

    /* unlike the info function, you can't keep track of the offsets without
     * extra allocations/VLAs, so we just have to go over the list twice. */

    const unsigned char* tmp_cursor = cursor;
    size_t buf_size = 0;
    for (uint16_t i = 0; i < rc; i++) {
        uint8_t len;

        memcpy(&len, tmp_cursor, 1);
        if (SIZE_MAX - rexp < (size_t)len) return -EOVERFLOW;
        rexp += len;
        if (rlen < rexp) return -EBADE;
        buf_size += (size_t)len + 1;
        tmp_cursor += len + 1;

        memcpy(&len, tmp_cursor, 1);
        if (SIZE_MAX - rexp < (size_t)len) return -EOVERFLOW;
        rexp += len;
        if (rlen < rexp) return -EBADE;
        buf_size += (size_t)len + 1;
        tmp_cursor += len + 1;
    }

    struct servrules* rules =
        malloc(sizeof(struct servrules) + sizeof(struct servrule) * rc);
    if (rules == NULL) return -errno;

    char* txt = malloc(buf_size);
    if (txt == NULL) {
        free(rules);
        return -errno;
    }

    rules->len = rc;
    rules->txt = txt;

    size_t buf_off = 0;
    for (uint16_t i = 0; i < rc; i++) {
        uint8_t len;

        memcpy(&len, cursor, 1);
        cursor++;
        rules->rules[i].name_off = buf_off;
        memcpy(txt + buf_off, cursor, len);
        buf_off += len;
        txt[buf_off++] = '\0';
        cursor += len;

        memcpy(&len, cursor, 1);
        cursor++;
        rules->rules[i].value_off = buf_off;
        memcpy(txt + buf_off, cursor, len);
        buf_off += len;
        txt[buf_off++] = '\0';
        cursor += len;
    }

    *out = rules;
    return 0;
}

/* IGNORES buf, parameter is kept solely for similar signature */
static int parse_clients(const unsigned char resp[DGRAM_MAX], const size_t rlen,
                         size_t rexp, struct servclients** out,
                         struct servquery_textbuf* buf) {
    if (out == NULL) return -EINVAL;
    /* this function completely ignores buf, as in my honest opinion it
     * currently makes NO SENSE for it to use a user provided buf. */
    (void)buf;

    const unsigned char* cursor = resp + QUERY_LEN;
    uint16_t cc;
    memcpy(&cc, cursor, 2);
    cursor += 2;

    /* at least 5 bytes per cc */
    if ((SIZE_MAX - rexp) / 5 < (size_t)cc) return -EOVERFLOW;
    rexp += cc * 5;

    if (rlen < rexp) return -EBADE;

    const unsigned char* tmp_cursor = cursor;
    size_t buf_size = 0;
    for (uint16_t i = 0; i < cc; i++) {
        uint8_t len;

        memcpy(&len, tmp_cursor, 1);
        if (SIZE_MAX - rexp < (size_t)len) return -EOVERFLOW;
        rexp += len;
        if (rlen < rexp) return -EBADE;
        buf_size += (size_t)len + 1;
        tmp_cursor += len + 1;

        /* skip over the score, should be already accouned for */
        tmp_cursor += 4;
    }

    struct servclients* clients =
        malloc(sizeof(struct servclients) + sizeof(struct servclient) * cc);
    if (clients == NULL) return -errno;
    char* txt = malloc(buf_size);
    if (txt == NULL) {
        free(clients);
        return -errno;
    }

    clients->len = cc;
    clients->txt = txt;

    size_t buf_off = 0;
    for (uint16_t i = 0; i < cc; i++) {
        uint8_t len;

        memcpy(&len, cursor, 1);
        cursor++;
        clients->clients[i].name_off = buf_off;
        memcpy(txt + buf_off, cursor, len);
        buf_off += len;
        txt[buf_off++] = '\0';
        cursor += len;

        memcpy(&clients->clients[i].score, cursor, 4);
        cursor += 4;
    }

    *out = clients;
    return 0;
}

int servquery_destroy(void) {
    if (sockfd < 0) return 0;

    while (close(sockfd) < 0) {
        if (errno != EINTR) return -errno;
    }

    sockfd = -1;
    return 0;
}
