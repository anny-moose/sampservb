#include "states.h"

#include <curses.h>
#include <stdlib.h>
#include <string.h>

#include "cmd.h"
#include "serv.h"

int init_tab(struct tab_state* out) {
    if (out == NULL) return -1;

    struct tab_state cfg = {
        .sort = FIELD_NAME,
        .filters = 0,
        .display = {~0, 60, 20, 20},
        .list = NULL,
    };

    static struct set_bounds uname = {
        .lower.usize = 3,
        .upper.usize = 24,
    };

    struct setmap map[] = {
        {
            .name = "exec_cmd",
            .target = &out->exec_cmd,
            .setfunc = set_charp,
        },
        {
            .name = "username",
            .target = out->username,
            .setfunc = set_fixedstr,
            .params = &uname,
        },
        {
            .name = "filter",
            .target = &out->filters,
            .setfunc = set_filter,
            /* .params = */
        },
        {
            .name = "shown",
            .target = &out->display.shown_fields,
            .setfunc = set_sort,
            .params = (void*)1,
        },
        {
            .name = "sort",
            .target = &out->sort,
            .setfunc = set_sort,
            /* .params = */
        },
    };

    struct setmap* dynmap = malloc(sizeof(map));
    if (dynmap == NULL) return -1;

    memcpy(dynmap, map, sizeof(map));

    cfg.map = dynmap;
    cfg.map_count = sizeof(map) / sizeof(map[0]);

    *out = cfg;

    return 0;
}

void free_tab(struct tab_state* target) {
    if (target == NULL) return;

    free(target->map);
    free(target->exec_cmd);

    servlist_free(target->list);
}
