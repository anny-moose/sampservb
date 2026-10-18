#ifndef SERVFETCH_H_
#define SERVFETCH_H_

#include "serv.h"

struct json_keys {
    char* pa_key; /* bool */
    char* ip_key; /* expected to be a string of format xxx.xxx.xxx.xxx:xxxxx */
    char* pc_key;
    char* pm_key;

    char* string_keys[3]; /* expected to be hostname, then gamemode, then
                             language */
};

struct servlist* fetch_servers(const char* url, struct json_keys keys);

enum servquery_opcode {
    SERVQUERY_OPCODE_INFO = 'i',
    SERVQUERY_OPCODE_RULES = 'r',
    SERVQUERY_OPCODE_CLIENTS = 'c',
    SERVQUERY_OPCODE_UNKNOWN,
};

struct servquery_textbuf {
    char* txt;
    size_t off;
    size_t size;
};

union servquery_out {
    struct servinfo info;
    struct servrules* rules;
    struct servclients* clients;
};

int servquery_init(void);
int servquery_sendreq(const struct sockaddr_in serv, char opcode);

int servquery_inspectresp(struct sockaddr_in* out_from);

bool servquery_respavail(void);

int servquery_getresp(union servquery_out* out, const struct sockaddr_in* from,
                      struct sockaddr_in* out_from,
                      enum servquery_opcode exp_op,
                      struct servquery_textbuf* buf, bool* out_consumed);

int servquery_getanyresp(union servquery_out* out, struct sockaddr_in* out_from,
                         struct servquery_textbuf* buf, bool* out_consumed);

int servquery_getfd(void);
int servquery_discardresp(void);
int servquery_destroy(void);

struct masterstate {
    uint8_t refs;
    size_t len;
    size_t next_entry;
    struct servquery_textbuf buf;
    void* userdata;
    struct sockaddr_in entries[];
};

struct masterstate* fetch_masterstate(const char* url);

#endif
