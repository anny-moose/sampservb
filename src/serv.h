#ifndef SERV_H_
#define SERV_H_

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct servinfo {
    struct in_addr ip;
    uint16_t port;
    uint16_t pc; /* player_count; max value is 1000 */
    uint16_t pm; /* player_max; max value is 1000 */
    bool pa;     /* password_required */

    /* text fields (offsets to the beginning of a zero-terminated string in
     * associated txt) */
    size_t hn_off;
    size_t gm_off;
    size_t ln_off;
    char* txt;
};

struct servrules {
    size_t len;
    char* txt;
    struct servrule {
        size_t name_off;
        size_t value_off;
    } rules[];
};

struct servlist {
    size_t len;
    size_t cap;
    size_t num_displayed; /* when filtering, all servers that should be hidden
                             should be after num_displayed */
    char* txt;
    struct servinfo servs[];
};

int servlist_hide(struct servlist* servers, size_t idx);
int sort_serverlist(struct servlist* servers, int8_t sort_field,
                    uint8_t filters, char* query);
int servlist_resize(struct servlist** servers, size_t new_cap);
void servlist_free(struct servlist* servs);

#endif
