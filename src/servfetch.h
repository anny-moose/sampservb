#ifndef SERVFETCH_H_
#define SERVFETCH_H_

#include "serv.h"

struct servlist* fetch_servers(const char* url);

int servquery_init(void);
int servquery_info(struct sockaddr_in serv, struct servinfo* out);
int servquery_destroy(void);

#endif
