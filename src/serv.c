#include "serv.h"

#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "states.h"

int servlist_hide(struct servlist* servers, size_t idx) {
    if (servers == NULL || idx < 0) return -1;
    if (idx >= servers->num_displayed) return 0;

    struct servinfo tmp = servers->servs[servers->num_displayed - 1];
    servers->servs[servers->num_displayed - 1] = servers->servs[idx];
    servers->servs[idx] = tmp;
    servers->num_displayed--;

    return 0;
}

static int8_t cmp_field;
static struct servlist* sorted_list;
static int servcmp(const void* s1_, const void* s2_) {
    const struct servinfo* s1 = s1_;
    const struct servinfo* s2 = s2_;

    const char* s1_txt = s1->txt != NULL ? s1->txt : sorted_list->txt;
    const char* s2_txt = s2->txt != NULL ? s2->txt : sorted_list->txt;

    int ret;
    if (cmp_field & FIELD_PR) {
        if (s1->pa == s2->pa)
            ret = 0;
        else if (s1->pa && !s2->pa)
            ret = 1;
        else
            ret = -1;
    } else if (cmp_field & FIELD_PC) {
        if (s1->pc == s2->pc)
            ret = 0;
        else if (s1->pc < s2->pc)
            ret = 1;
        else
            ret = -1;
    } else if (s1_txt == NULL || s2_txt == NULL) {
        ret = 0;
    } else if (cmp_field & FIELD_NAME) {
        ret = strcmp(s1_txt + s1->hn_off, s2_txt + s2->hn_off);
    } else if (cmp_field & FIELD_GM) {
        ret = strcmp(s1_txt + s1->gm_off, s2_txt + s2->gm_off);
    } else if (cmp_field & FIELD_LN) {
        ret = strcmp(s1_txt + s1->ln_off, s2_txt + s2->ln_off);
    }

    return (cmp_field < 0) ? -ret : ret;
}

int servlist_resize(struct servlist** servers, size_t new_cap) {
    if (servers == NULL || (*servers != NULL && (*servers)->len > new_cap))
        return -1;

    struct servlist* new_servers = realloc(
        *servers, sizeof(struct servlist) + sizeof(struct servinfo) * new_cap);
    if (new_servers == NULL) return -1;

    if (*servers == NULL) *new_servers = (struct servlist){0};

    new_servers->cap = new_cap;

    *servers = new_servers;
    return 0;
}

/* on non-glibc platforms this swaps the first and last elements when every
 * element is equal according to the sort function */
int sort_serverlist(struct servlist* servers, int8_t sort_field,
                    uint8_t filters, char* query) {
    if (servers == NULL) return -1;
    servers->num_displayed = servers->len;
    for (size_t i = 0; i < servers->num_displayed;) {
        const char* txt = servers->servs[i].txt != NULL ? servers->servs[i].txt
                                                        : servers->txt;

        // clang-format off
        if ((servers->servs[i].pa && filters & HIDE_PR) ||
            (servers->servs[i].pc == 0 && filters & HIDE_EMPTY) ||
            (servers->servs[i].pc == servers->servs[i].pm && filters & HIDE_FULL) ||
            (txt != NULL && query != NULL && strstr(txt + servers->servs[i].hn_off, query) == NULL)) {
            // clang-format on
            servlist_hide(servers, i);
        } else {
            i++;
        }
    }

    if (sort_field == 0) sort_field = FIELD_NAME;

    cmp_field = sort_field;
    sorted_list = servers;
    qsort(servers->servs, servers->num_displayed, sizeof(struct servinfo),
          servcmp);

    return 0;
}

void servlist_free(struct servlist* servs) {
    if (servs == NULL) return;

    for (size_t i = 0; i < servs->len; i++) {
        free(servs->servs[i].txt);
    }
    free(servs->txt);

    free(servs);
}
