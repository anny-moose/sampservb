#ifndef STATES_H_
#define STATES_H_

#include <curses.h>
#include <unistd.h>

#include "cmd.h"

#define FIELD_PR 0x1
#define FIELD_NAME 0x2
#define FIELD_PC 0x4
#define FIELD_GM 0x8
#define FIELD_LN 0x10

struct display_cfg {
    uint8_t shown_fields;
    uint16_t name_cols;
    uint16_t gm_cols;
    uint16_t ln_cols;
};

#define HIDE_PR 0x1
#define HIDE_EMPTY 0x2
#define HIDE_FULL 0x4

struct tab_state {
    char username[25];
    char* exec_cmd;
    uint8_t filters;
    int8_t sort;
    int8_t visual_sort;
    size_t selected;
    struct display_cfg display;
    struct servlist* list;

    size_t map_count;
    struct setmap* map;

    char search_buf[64];
};

struct app_state {
    bool quit;
    pid_t game_proc;

    /* 95 printable ascii characters, : and / will be used for \n and \t */
    struct keymapping* keycmd[95];

    size_t tabs_selected;
    struct tab_state* tabs;
    size_t tabs_count;
    size_t tabs_capacity;

    WINDOW* const servlist_win;
};

int init_tab(struct tab_state* out);
void free_tab(struct tab_state* target);

#endif
