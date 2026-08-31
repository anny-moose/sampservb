/* TODO: rewrite this because of shady/poor code quality and hardcoded api */

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

enum dispatcher {
    DISPATCH_INT = 0,
    DISPATCH_STR = 1,
    DISPATCH_BOOL = 2,
    DISPATCH_IP = 4,
    DISPATCH_NONE = 5,
};
/* signature: X(name_serverside, name_struct, type, dispatcher) */

#define SERVER_FIELDS                       \
    X(ip, ip, struct in_addr, DISPATCH_IP)  \
    X(_port, port, uint16_t, DISPATCH_NONE) \
    X(pc, pc, uint16_t, DISPATCH_INT)       \
    X(pm, pm, uint16_t, DISPATCH_INT)       \
    X(pa, pa, bool, DISPATCH_BOOL)          \
    X(hn, hn_off, size_t, DISPATCH_STR)     \
    X(gm, gm_off, size_t, DISPATCH_STR)     \
    X(la, ln_off, size_t, DISPATCH_STR)

#define ASSERT_INT(key, entry, fail_label)        \
    do {                                          \
        (key) = cJSON_GetObjectItem(entry, #key); \
        if (!cJSON_IsNumber((key))) {             \
            warnx(#key "isn't a number! Fail.");  \
            goto fail_label;                      \
        }                                         \
    } while (0)

#define ASSERT_BOOL(key, entry, fail_label)       \
    do {                                          \
        (key) = cJSON_GetObjectItem(entry, #key); \
        if (!cJSON_IsBool((key))) {               \
            warnx(#key "isn't a bool! Fail.");    \
            goto fail_label;                      \
        }                                         \
    } while (0)

#define GET_STRING(key, entry, buf_size, fail_label)  \
    do {                                              \
        (key) = cJSON_GetObjectItem((entry), #key);   \
        if (!cJSON_IsString((key))) {                 \
            warnx(#key "isn't a string! Fail.");      \
            goto fail_label;                          \
        }                                             \
        (buf_size) += strlen((key)->valuestring) + 1; \
    } while (0)

#define BUF_SIZE 4096
static int parse_servers(const char* json, struct servlist** out) {
    int ret = -1;

    if (json == NULL) {
        warnx("json passed to parse_servers was NULL");
        return ret;
    }

    cJSON* parsed = cJSON_Parse(json);
    if (parsed == NULL) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            warnx("Json parsing error: %s", error_ptr);
        }
        goto end;
    }

#define X(name_serverside, name_struct, type, dispatcher) \
    const cJSON*(name_serverside) = NULL;
    SERVER_FIELDS
#undef X

#define X(name_serverside, name_struct, type, dispatcher) \
    type name_struct##_local;
    SERVER_FIELDS
#undef X

    size_t arr_size = cJSON_GetArraySize(parsed);
    if (out == NULL) return arr_size;

    size_t alloc_size =
        sizeof(struct servlist) + sizeof(struct servinfo) * arr_size;
    struct servlist* list = malloc(alloc_size);
    if (list == NULL) {
        warn("Malloc of %zu bytes failed", alloc_size);
        goto end;
    }
    list->cap = arr_size;

    size_t server_count = 0;
    char* textbuf = malloc(BUF_SIZE);
    if (textbuf == NULL) {
        warnx("Failed to allocate textbuf");
        goto fail;
    }

    size_t buf_used = 0;
    size_t buf_size = BUF_SIZE;
    const cJSON* entry = NULL;
    char* tmp = NULL;
    cJSON_ArrayForEach(entry, parsed) {
#define X(name_serverside, name_struct, type, dispatcher)                   \
    switch (dispatcher) {                                                   \
        case DISPATCH_INT:                                                  \
            ASSERT_INT(name_serverside, entry, fail);                       \
            name_struct##_local = (type){name_serverside->valuedouble};     \
            break;                                                          \
        case DISPATCH_BOOL:                                                 \
            ASSERT_BOOL(name_serverside, entry, fail);                      \
            name_struct##_local = (type){cJSON_IsTrue(name_serverside)};    \
            break;                                                          \
        case DISPATCH_IP:                                                   \
            name_serverside = cJSON_GetObjectItem(entry, #name_serverside); \
            if (!cJSON_IsString(name_serverside)) {                         \
                warnx(#name_serverside "isn't a string! Fail.");            \
                goto fail;                                                  \
            }                                                               \
            tmp = strchr(name_serverside->valuestring, ':');                \
            if (tmp == NULL) {                                              \
                warnx("Failed to read port");                               \
                goto fail;                                                  \
            }                                                               \
            port_local = htons(atoi(tmp + 1));                              \
            *tmp = '\0';                                                    \
            if (inet_pton(AF_INET, name_serverside->valuestring,            \
                          &(name_struct##_local)) != 1) {                   \
                warnx("Failed to read IP");                                 \
                goto fail;                                                  \
            }                                                               \
            *tmp = ':';                                                     \
                                                                            \
            /* TODO: IPPORT!!! */                                           \
            break;                                                          \
        case DISPATCH_STR:                                                  \
            name_struct##_local = (type){buf_used};                         \
            GET_STRING(name_serverside, entry, buf_used, fail);             \
            break;                                                          \
        case DISPATCH_NONE:                                                 \
            break;                                                          \
    }

        SERVER_FIELDS
#undef X

        if (buf_used > buf_size) {
            while (buf_used > buf_size) buf_size += buf_size / 2;

            char* new_buf = realloc(textbuf, buf_size);
            if (new_buf == NULL) {
                warn("failed to (re)allocate text buffer of size %zu",
                     buf_size);
            fail:
                free(textbuf);
                free(list);
                goto end;
            }
            textbuf = new_buf;
        }

#define X(name_serverside, name_struct, type, dispatcher) \
    list->servs[server_count].name_struct = name_struct##_local;
        SERVER_FIELDS
#undef X

        list->servs[server_count].txt = NULL;

        strcpy(textbuf + hn_off_local, hn->valuestring);
        strcpy(textbuf + gm_off_local, gm->valuestring);
        strcpy(textbuf + ln_off_local, la->valuestring);
        server_count++;
    }

    list->len = server_count;
    list->num_displayed = server_count;
    list->txt = textbuf;

    ret = 0;
    *out = list;

end:
    cJSON_Delete(parsed);
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

struct servlist* fetch_servers(const char* url) {
    struct servlist* list;
    char* raw = get_resp(url);
    if (raw == NULL) return NULL;
    if (parse_servers(raw, &list) < 0) {
        free(raw);
        return NULL;
    }

    free(raw);
    return list;
}

static int sockfd;
int servquery_init(void) {
    if (sockfd != 0) {
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    sockfd = fd;
    return 0;
}

#define DGRAM_MAX 65535
#define QUERY_LEN 11
int servquery_info(struct sockaddr_in serv, struct servinfo* out) {
    if (sockfd == 0) return -1;

    unsigned char resp[DGRAM_MAX];
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

    req[10] = 'i';

    alarm(5);
    if (sendto(sockfd, req, QUERY_LEN, 0, (struct sockaddr*)&serv,
               sizeof(serv)) < 0)
        return errno == EINTR ? -2 : -1;
    alarm(0);

    alarm(5);
    ssize_t ret;
    if ((ret = recvfrom(sockfd, resp, DGRAM_MAX, 0, NULL, NULL)) < 0)
        return errno == EINTR ? -3 : -1;
    alarm(0);

    /* https://open.mp/docs/tutorials/QueryMechanism#response */
    size_t exp =
        QUERY_LEN + 17; /* all non-variable-length fields sum up to 17. */

    /* safe cast because ret can't be < 0 */
    if ((size_t)ret < exp) return -1;

    if (memcmp(resp, req, QUERY_LEN) != 0) return -1;

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

int servquery_destroy(void) {
    if (sockfd == 0) return 1;

    while (close(sockfd) < 0) {
        if (errno != EINTR) return -1;
    }

    sockfd = 0;
    return 0;
}
