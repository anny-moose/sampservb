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

struct servlist* fetch_servers(const char* url);

int servquery_init(void);
int servquery_info(struct sockaddr_in serv, struct servinfo* out);
int servquery_destroy(void);

#endif
