#include "states.h"

#include <curses.h>
#include <stdlib.h>
#include <string.h>

#include "serv.h"

int init_tab(struct tab_state* out) {
    if (out == NULL) return -1;

    struct tab_state cfg = {
        .sort = FIELD_NAME,
        .visual_sort = FIELD_NAME,
        .filters = 0,
        .display = {~0, 60, 20, 20},
        .list = NULL,
    };

    *out = cfg;

    return 0;
}

void free_tab(struct tab_state* target) {
    if (target == NULL) return;

    free(target->map);
    free(target->exec_cmd);

    servlist_free(target->list);
}
