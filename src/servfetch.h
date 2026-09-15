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
struct servlist* fetch_masterlist(const char* url);

int servquery_init(void);
int servquery_binfo(const struct sockaddr_in serv, struct servinfo* out,
                    char** buf, size_t* buf_offset, size_t* buf_size);
int servquery_info(const struct sockaddr_in serv, struct servinfo* out,
                   bool ignstr);
int servquery_rules(const struct sockaddr_in serv, struct servrules** out);
int servquery_clients(const struct sockaddr_in serv, struct servclients** out);
int servquery_destroy(void);

#endif
