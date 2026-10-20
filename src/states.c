#include "states.h"

#include <asm-generic/errno-base.h>
#include <curses.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "serv.h"
#include "vector.h"

int tab_init(struct rc_tab* out) {
    if (out == NULL) return -1;

    struct tab_state cfg = {
        .sort = FIELD_NAME,
        .visual_sort = FIELD_NAME,
        .filters = 0,
        /* 19 preoccupied, 61 left */
        .display = {~0, 40, 15, 11},
        .list = NULL,
    };

    *out = (struct rc_tab){
        .tab = cfg,
        .refs = 1,
        .markfree = false,
    };

    return 0;
}

int tab_wait(struct rc_tab* tab) {
    if (tab == NULL) return -EINVAL;
    if (tab->refs >= TAB_MAXREFS) abort();

    tab->refs++;
    return 0;
}

void tab_post(struct rc_tab* target) {
    if (target == NULL) return;

    if (target->refs == 1) {
        free(target->tab.map);
        free(target->tab.exec_cmd);
        servlist_free(target->tab.list);

        memset(target, 0, sizeof(*target));
    }

    if (target->refs >= 1) {
        target->refs--;
    }
}

static struct vector plist_vec(struct packet_list* list) {
    return (struct vector){
        .data = list->packets,
        .len = list->len,
        .cap = list->cap,
        .ds = sizeof(*list->packets),
    };
}

static void vec_plist(struct vector* vec, struct packet_list* into) {
    into->packets = vec->data;
    into->len = vec->len;
    into->cap = vec->cap;
}

int packet_list_init(struct packet_list* list) {
    if (list == NULL) return -EINVAL;
    struct vector vec = {0};

    int ret = vector_init(&vec, 16, sizeof(*list->packets));
    if (ret < 0) return ret;

    vec_plist(&vec, list);
    list->accepting = true;
    gettimeofday(&list->prevtime, NULL);

    return 0;
}

int packet_list_addpacket(struct packet_list* list,
                          struct expected_packet* packet) {
    if (list == NULL) return -EINVAL;
    if (list->accepting == false) return -EACCES;

    int ret = packet_list_passtime(list, NULL);
    if (ret < 0) return ret;

    struct vector vec = plist_vec(list);
    ret = vector_push(&vec, packet);
    if (ret < 0) return ret;

    vec_plist(&vec, list);
    return 0;
}

static int tval_ms(struct timeval tval) {
    int ms = tval.tv_usec / 1000;

    int rsecs = (INT_MAX - ms) / 1000;
    if (tval.tv_sec > rsecs)
        return INT_MAX;
    else
        return ms + tval.tv_sec * 1000;
}

int packet_list_getmintimeout_ms(const struct packet_list* list) {
    if (list == NULL || list->packets == NULL || list->len == 0) return -1;

    int ret = INT_MAX;
    for (size_t i = 0; i < list->len; i++) {
        int curr = tval_ms(list->packets[i].timeout);
        if (curr < ret) ret = curr;
    }

    return ret;
}

static struct timeval timesub(struct timeval from, struct timeval of) {
    struct timeval ret = {0};

    if (of.tv_sec > from.tv_sec) return ret;

    ret.tv_sec = from.tv_sec - of.tv_sec;

    if (of.tv_usec > from.tv_usec) {
        if (ret.tv_sec == 0) return ret;
        ret.tv_sec--;
        ret.tv_usec = 1000000 - of.tv_usec + from.tv_usec;
        return ret;
    } else {
        ret.tv_usec = from.tv_usec - of.tv_usec;
        return ret;
    }
}

int packet_list_passtime(struct packet_list* list, size_t* firsti) {
    if (list == NULL) return -EINVAL;

    struct timeval time, passed;
    gettimeofday(&time, NULL);

    passed = timesub(time, list->prevtime);

    for (size_t i = 0; i < list->len; i++) {
        struct timeval* curr = &list->packets[i].timeout;
        *curr = timesub(*curr, passed);

        if (curr->tv_sec == 0 && curr->tv_usec == 0) {
            if (firsti != NULL) {
                *firsti = i;
                firsti = NULL;
            }
        }
    }

    if (firsti != NULL) *firsti = list->len;
    list->prevtime = time;

    return 0;
}

int packet_list_popmatch(struct packet_list* list, struct sockaddr_in from,
                         enum servquery_opcode op,
                         struct expected_packet* out) {
    if (list == NULL || out == NULL) return -EINVAL;
    for (size_t i = 0; i < list->len; i++) {
        if (memcmp(&from, &list->packets[i].from, sizeof(from)) != 0
            || memcmp(&op, &list->packets[i].op, sizeof(op)) != 0)
            continue;

        struct vector vec = plist_vec(list);
        int ret = vector_swap_pop(&vec, i, out);
        if (ret < 0) return ret;
        vec_plist(&vec, list);
        return 0;
    }

    return -ENOENT;
}

int packet_list_popexpired(struct packet_list* list, size_t* startidx,
                           struct expected_packet* out) {
    if (list == NULL || out == NULL) return -EINVAL;
    size_t idx = (startidx == NULL) ? 0 : *startidx;
    if (idx >= list->len) return -ENOENT;

    for (; idx < list->len; idx++) {
        struct timeval tout = list->packets[idx].timeout;
        if (tout.tv_sec != 0 || tout.tv_usec != 0) continue;

        struct vector vec = plist_vec(list);
        if (startidx != NULL) *startidx = idx;
        vector_swap_pop(&vec, idx, out);
        vec_plist(&vec, list);
        return 0;
    }

    if (startidx != NULL) *startidx = list->len;

    return -ENOENT;
}

/* stop accepting new packets and treat all old ones as if they expired */
int packet_list_shutdown_sequence(struct packet_list* list) {
    if (list == NULL) return -EINVAL;

    struct expected_packet leftover;

    list->accepting = false;
    struct vector vec = plist_vec(list);

    while (vec.len > 0) {
        vector_swap_pop(&vec, vec.len - 1, &leftover);

        leftover.tact(&leftover, list);
    }

    vec_plist(&vec, list);

    return 0;
}

int packet_list_destroy(struct packet_list* list) {
    if (list == NULL || list->len > 0) return -EINVAL;

    struct vector vec = plist_vec(list);
    vector_destroy(&vec);
    memset(list, 0, sizeof(struct packet_list));
    return 0;
}

/* test whether all tabs in a list can be relocated */
static bool tabs_testflex(const struct tabs* tabs) {
    if (tabs == NULL) return false;

    size_t tabs_reffed = 0;
    size_t refs = 0;

    for (size_t i = 0; i < tabs->cap; i++) {
        if (tabs->inds[i] != SIZE_MAX) tabs_reffed++;

        if (tabs->tabs[i].refs > 1)
            return false;
        else {
            refs += tabs->tabs[i].refs;
        }
    }

    if (tabs_reffed > refs)
        abort(); /* shouldn't happen; indicative of state being messed up */

    return refs > tabs_reffed ? false : true;
}

int tabs_reallocate(struct tabs* tabs, size_t new_size) {
    /* no support for resizing downwards because typical use case doesn't need
     * it */
    if (tabs == NULL || new_size < tabs->cap) return -EINVAL;

    if (new_size == tabs->cap) return 0;

    if (tabs_testflex(tabs) == false) return -EINVAL;

    size_t size = tabs->cap;

    if (SIZE_MAX / sizeof(*tabs->tabs) < new_size) return -EOVERFLOW;
    if (SIZE_MAX / sizeof(int) < new_size) return -EOVERFLOW;

    /* TWO allocations. preposterous */
    struct rc_tab* newtabs = malloc(sizeof(*tabs->tabs) * new_size);
    size_t* newinds = malloc(sizeof(size_t) * new_size);

    if (newtabs == NULL || newinds == NULL) {
        free(newtabs);
        free(newinds);
        return -errno;
    }

    /* manpage says nothing about copying of zero bytes so use an if even though
     * it's probably redundant */
    if (tabs->tabs != NULL)
        memcpy(newtabs, tabs->tabs, sizeof(*newtabs) * tabs->cap);
    if (tabs->inds != NULL)
        memcpy(newinds, tabs->inds, sizeof(*newinds) * tabs->cap);

    free(tabs->tabs);
    free(tabs->inds);

    memset(newinds + size, 0xFF, sizeof(size_t) * (new_size - size));
    memset(newtabs + size, 0, sizeof(*newtabs) * (new_size - size));

    tabs->tabs = newtabs;
    tabs->inds = newinds;
    tabs->cap = new_size;

    return 0;
}

struct rc_tab* app_getcurrtab(struct app_state* state) {
    if (state == NULL || state->tabs.sel >= state->tabs.cap) return NULL;
    size_t idx = state->tabs.inds[state->tabs.sel];
    if (idx >= state->tabs.cap) return NULL;

    struct rc_tab* tab = state->tabs.tabs + idx;
    if (tab->refs == 0) return NULL;

    return tab;
}

struct tab_state* app_getcurrtab_raw(struct app_state* state) {
    struct rc_tab* tab = app_getcurrtab(state);
    if (tab == NULL) return NULL;

    return &tab->tab;
}

size_t app_getnexttab(struct app_state* state) {
    for (size_t i = state->tabs.sel + 1; i < state->tabs.cap; i++) {
        if (state->tabs.inds[i] != SIZE_MAX) return i;
    }

    return SIZE_MAX;
}

size_t app_getprevtab(struct app_state* state) {
    if (state->tabs.sel == 0) return SIZE_MAX;

    for (size_t i = state->tabs.sel - 1; i != SIZE_MAX; i--) {
        if (state->tabs.inds[i] != SIZE_MAX) return i;
    }

    return SIZE_MAX;
}

int tabs_newtab(struct tabs* tabs, const char* name, size_t nlen) {
    if (nlen >= TABNAME_LEN) return -EINVAL;

    size_t fidx = SIZE_MAX;
    for (size_t i = 0; i < tabs->cap; i++) {
        if (tabs->tabs[i].refs == 0) {
            fidx = i;
            break;
        }
    }

    if (fidx == SIZE_MAX) return -ENOSPC;

    int ret = tab_init(tabs->tabs + fidx);
    if (ret < 0) return ret;

    for (size_t i = 0; i < tabs->cap; i++) {
        if (tabs->inds[i] != SIZE_MAX) continue;

        tabs->inds[i] = fidx;
        tabs->sel = i;
        if (name == NULL) {
            snprintf(tabs->tabs[fidx].tab.tab_name, TABNAME_LEN, "Tab %zu",
                     fidx + 1);
        } else {
            memcpy(tabs->tabs[fidx].tab.tab_name, name, nlen);
            tabs->tabs[fidx].tab.tab_name[nlen] = '\0';
        }

        break;
    }

    return 0;
}
