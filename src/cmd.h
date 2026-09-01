#ifndef CMD_H_
#define CMD_H_

#include <stddef.h>

#include "states.h"

/* Stuff for setting up the set mappings */

union set_bound {
    size_t usize;
    long ilong;
};

struct set_bounds {
    union set_bound lower;
    union set_bound upper;
};

struct set_num_params {
    enum {
        SET_NUM_U8,
    } num_type;
    struct set_bounds bounds;
};

/*
void set_fixedstr(void* target, const char* setting, void* params);
void set_charp(void* target, const char* setting, void* params);
void set_num(void* target, const char* setting, void* params);
void set_filter(void* target, const char* setting, void* params);
void set_sort(void* target, const char* setting, void* params);
*/

struct setmap {
    const char* name;
    size_t offset;
    void (*setfunc)(void* target, const char* setting, const void* params);
    const void* params;
};

/* --Stuff for setting up the set mappings */

void handle_cmd(char* cmd, struct app_state* cfg);

#endif
