#ifndef KEYMAP_H_
#define KEYMAP_H_
#include <stddef.h>

#include "cmd.h"

struct keymapping;
struct keymap {
    struct keymapping** data;
    size_t len;
    size_t cap;
};

int keymap_strtokey(const char* str);

int keymap_init(struct keymap* map, size_t initial_cap);
int keymap_del(struct keymap* map, int key);
int keymap_set(struct keymap* map, int key, const char* cmd, const char* args);
const struct keymapping* keymap_get(const struct keymap* map, int key);
void keymap_destroy(struct keymap* map);
sidefx keymap_call(const struct keymap* map, int key, struct app_state* cfg);

#endif
