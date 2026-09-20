#include "keymap.h"

#include <curses.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cmd.h"

struct keycodemap {
    const char* name;
    int key;
} keys[] = {
    {"<Space>", ' '},       {"<CR>", '\n'},   {"<Tab>", '\t'},
    {"<Right>", KEY_RIGHT}, {"<Up>", KEY_UP}, {"<Down>", KEY_DOWN},
    {"<Left>", KEY_LEFT},
};

int keymap_strtokey(const char* str) {
    if (str == NULL) return ERR;

    if (str[1] == '\0') {
        if (*str < 32 || *str > 127 || *str == ':' || *str == '\\') return ERR;

        return *str;
    }

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(str, keys[i].name) == 0) return keys[i].key;
    }

    return ERR;
}

struct keymapping {
    int key;
    sidefx (*call)(const char**, void*);
    uint8_t lvl;
    char* argv[];
};

static void mapping_construct_argv(char** argv, size_t count, const char* cmd,
                                   const char* tokstr) {
    if (count > 0) {
        char* args = stpcpy((char*)(argv + count), cmd) + 1;
        argv[0] = (char*)(argv + count);
        if (tokstr != NULL) {
            stpcpy(args, tokstr);
            parse_toks(args, argv + 1, count - 1);
        }
    }
}

static int create_keymapping(int key, const char* cmd, const char* tokstr,
                             struct keymapping** out) {
    size_t alloc_size = sizeof(struct keymapping);

    const struct regcmd* found = get_cmd(cmd);
    if (found == NULL) return -2;

    if (found->expected_args != 0) {
        alloc_size += sizeof(char*) * found->expected_args;
        alloc_size += strlen(cmd) + 1;
        if (tokstr != NULL) {
            alloc_size += strlen(tokstr) + 1;
        }
    }

    struct keymapping* newmap = calloc(1, alloc_size);
    if (newmap == NULL) return -1;

    newmap->call = found->call;
    newmap->lvl = found->lvl;
    newmap->key = key;
    mapping_construct_argv(newmap->argv, found->expected_args, cmd, tokstr);

    *out = newmap;
    return 0;
}

static int keymappingcmp(const void* m1_, const void* m2_) {
    if (m1_ == NULL || m2_ == NULL) abort();

    const struct keymapping* m1 = *(const struct keymapping**)m1_;
    const struct keymapping* m2 = *(const struct keymapping**)m2_;
    if (m1 == NULL || m2 == NULL) abort();

    if (m1->key > m2->key)
        return 1;
    else if (m1->key < m2->key)
        return -1;
    else
        return 0;
}

int keymap_init(struct keymap* map, size_t initial_cap) {
    if (map == NULL) return -1;

    struct keymapping** new_data = NULL;
    if (initial_cap > 0) {
        new_data = malloc(sizeof(map->data[0]) * initial_cap);
        if (new_data == NULL) return -1;
    }

    map->data = new_data;
    map->len = 0;
    map->cap = initial_cap;

    return 0;
}

const struct keymapping* keymap_get(const struct keymap* map, int key) {
    if (map == NULL || map->data == NULL) return NULL;
    struct keymapping* cmp = &(struct keymapping){
        .key = key,
    };

    const struct keymapping** mapping =
        bsearch(&cmp, map->data, map->len, sizeof(*map->data), keymappingcmp);
    if (mapping == NULL) return NULL;

    return *mapping;
}

/* find index of the first element with higher key value than targkey, or one
 * past last if not found */
static size_t bs_findhi(const struct keymap* map, int targkey) {
    size_t ceil = map->len;
    size_t floor = 0;
    while (floor < ceil) {
        size_t i = floor + (ceil - floor) / 2;
        if (map->data[i]->key > targkey) {
            ceil = i;
        } else {
            floor = i + 1;
        }
    }

    return floor;
}

int keymap_del(struct keymap* map, int key) {
    if (map == NULL || map->data == NULL) return -1;
    size_t highi = bs_findhi(map, key);

    /* mapping isn't present */
    if (highi == 0 || map->data[highi - 1]->key != key) return 0;

    free(map->data[highi - 1]);
    memmove(map->data + highi - 1, map->data + highi,
            sizeof(struct keymapping*) * (map->len - highi));
    map->len--;
    return 0;
}

int keymap_set(struct keymap* map, int key, const char* cmd,
               const char* tokstr) {
    if (map == NULL || cmd == NULL) return -1;

    size_t highi = bs_findhi(map, key);

    struct keymapping* newmap;
    int err = create_keymapping(key, cmd, tokstr, &newmap);
    if (err != 0) return err;

    /* just overwrite old mapping if one exists */
    if (highi > 0 && map->data[highi - 1]->key == key) {
        free(map->data[highi - 1]);
        map->data[highi - 1] = newmap;

        return 0;
    }

    if (map->cap <= map->len) {
        if (SIZE_MAX - map->cap < map->cap / 2) return -1;
        size_t newcap = (map->cap < 2) ? 2 : map->cap + map->cap / 2;

        void* newdata = realloc(map->data, sizeof(map->data[0]) * newcap);
        if (newdata == NULL) return -1;
        map->cap = newcap;
        map->data = newdata;
    }

    memmove(map->data + highi + 1, map->data + highi,
            sizeof(map->data[0]) * (map->len - highi));

    map->len++;
    map->data[highi] = newmap;

    return 0;
}

sidefx keymap_call(const struct keymap* map, int key, struct app_state* cfg) {
    const struct keymapping* mapping = keymap_get(map, key);
    if (mapping == NULL) return 0;

    return mapping->call((const char**)mapping->argv,
                         lvl_pointer(mapping->lvl, cfg));
}

void keymap_destroy(struct keymap* map) {
    if (map == NULL) return;

    for (size_t i = 0; i < map->len; i++) free(map->data[i]);

    free(map->data);
    map->data = NULL;
    map->len = 0;
    map->cap = 0;
}
