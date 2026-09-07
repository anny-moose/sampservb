#include "servfetch.h"

#include <arpa/inet.h>
#include <cJSON.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <err.h>
#include <errno.h>
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
        /* out of memory! */
        warn("not enough memory");
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
            warnx("Json parsing error: %s", error_ptr);
        }
        ret = -2;
        goto end;
    }

    if (!cJSON_IsArray(parsed)) {
        warnx("Server list isn't an array");
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
        warn("Malloc of %zu bytes failed", alloc_size);
        goto end_free;
    }
    list->cap = arr_size;

    size_t server_count = 0;
    char* textbuf = malloc(BUF_SIZE);
    if (textbuf == NULL) {
        warnx("Failed to allocate textbuf");
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
        warnx("curl init failed");
        return NULL;
    }

    CURLcode resp;

    resp = curl_easy_setopt(curl, CURLOPT_URL, url);

    resp = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_cb);
    resp = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json);

    resp = curl_easy_perform(curl);
    if (resp != CURLE_OK) {
        warnx("Failed to retrieve %s: %s\n", url, curl_easy_strerror(resp));
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

static int sockfd = -1;
int servquery_init(void) {
    if (sockfd >= 0) {
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    sockfd = fd;
    return 0;
}

#define DGRAM_MAX 65535
#define QUERY_LEN 11
static int servquery_sendreq(const struct sockaddr_in serv, char opcode,
                             unsigned char resp[DGRAM_MAX], size_t* exp,
                             ssize_t* ret) {
    if (sockfd < 0) return -1;

    unsigned char req[QUERY_LEN];

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

    size_t lexp;
    /* https://open.mp/docs/tutorials/QueryMechanism#response */
    switch (opcode) {
        case 'i':
            lexp = QUERY_LEN +
                   17; /* all non-variable-length fields sum up to 17. */
            break;
        case 'r':
            lexp = QUERY_LEN + 2;
            break;
        default:
            return -4;
    }

    req[10] = opcode;

    alarm(5);
    if (sendto(sockfd, req, QUERY_LEN, 0, (struct sockaddr*)&serv,
               sizeof(serv)) < 0)
        return errno == EINTR ? -2 : -1;
    alarm(0);

    alarm(5);
    ssize_t lret;
    if ((lret = recvfrom(sockfd, resp, DGRAM_MAX, 0, NULL, NULL)) < 0)
        return errno == EINTR ? -3 : -1;
    alarm(0);

    /* safe cast because ret can't be < 0 */
    if ((size_t)lret < lexp) return -1;
    if (memcmp(resp, req, QUERY_LEN) != 0) return -1;

    *exp = lexp;
    *ret = lret;

    return 0;
}

int servquery_info(const struct sockaddr_in serv, struct servinfo* out) {
    unsigned char resp[DGRAM_MAX];
    size_t exp;
    ssize_t ret;
    int err = servquery_sendreq(serv, 'i', resp, &exp, &ret);
    if (err < 0) return err;

    unsigned char* curs = resp + QUERY_LEN;
    struct servinfo info = {0};
    memcpy(&info.pa, curs, 1);
    curs++;

    memcpy(&info.pc, curs, 2);
    curs += 2;

    memcpy(&info.pm, curs, 2);
    curs += 2;

    /* calculate buffer size */

    size_t offsets[3];
    uint32_t lengths[3];
    size_t buf_size = 0;
    /* use another variable instead of then backtracking curs */
    unsigned char* curs_tmp = curs;
    for (size_t i = 0; i < 3; i++) {
        memcpy(lengths + i, curs_tmp, 4);
        if (lengths[i] > SIZE_MAX - exp) return -1;

        exp += lengths[i];
        if ((size_t)ret < exp) return -1;

        offsets[i] = buf_size;
        buf_size += lengths[i] + 1;
        curs_tmp += 4 + lengths[i];
    }

    char* textbuf = malloc(buf_size);
    if (textbuf == NULL) return -1;

    for (size_t i = 0; i < 3; i++) {
        curs += 4;
        memcpy(textbuf + offsets[i], curs, lengths[i]);
        textbuf[offsets[i] + lengths[i]] = '\0';
        curs += lengths[i];
    }

    info.hn_off = offsets[0];
    info.gm_off = offsets[1];
    info.ln_off = offsets[2];

    info.txt = textbuf;

    info.ip = serv.sin_addr;
    info.port = serv.sin_port;

    *out = info;
    return 0;
}

int servquery_rules(const struct sockaddr_in serv, struct servrules** out) {
    unsigned char resp[DGRAM_MAX];
    size_t exp;
    ssize_t ret;
    int err = servquery_sendreq(serv, 'r', resp, &exp, &ret);
    if (err < 0) return err;

    unsigned char* cursor = resp + QUERY_LEN;
    uint16_t rc;
    memcpy(&rc, cursor, 2);
    cursor += 2;

    /* check to make sure that we are not overflowing exp by adding rc * 2 to
     * it. (length of a rule and a value is described with a single byte) */
    if ((SIZE_MAX - exp) / 2 < (size_t)rc) return -1;
    exp += rc * 2;

    if ((size_t)ret < exp) return -1;

    /* unlike the info function, you can't keep track of the offsets without
     * extra allocations/VLAs, so we just have to go over the list twice. */

    unsigned char* tmp_cursor = cursor;
    size_t buf_size = 0;
    for (uint16_t i = 0; i < rc; i++) {
        uint8_t len;

        memcpy(&len, tmp_cursor, 1);
        if (SIZE_MAX - exp < (size_t)len) return -1;
        exp += len;
        if ((size_t)ret < exp) return -1;
        buf_size += (size_t)len + 1;
        tmp_cursor += len + 1;

        memcpy(&len, tmp_cursor, 1);
        if (SIZE_MAX - exp < (size_t)len) return -1;
        exp += len;
        if ((size_t)ret < exp) return -1;
        buf_size += (size_t)len + 1;
        tmp_cursor += len + 1;
    }

    struct servrules* rules =
        malloc(sizeof(struct servrules) + sizeof(struct servrule) * rc);
    if (rules == NULL) return -1;
    char* txt = malloc(buf_size);
    if (txt == NULL) {
        free(rules);
        return -1;
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

int servquery_destroy(void) {
    if (sockfd < 0) return 1;

    while (close(sockfd) < 0) {
        if (errno != EINTR) return -1;
    }

    sockfd = -1;
    return 0;
}
