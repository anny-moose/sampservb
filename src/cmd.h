#ifndef CMD_H_
#define CMD_H_

#include <stddef.h>
#include <stdint.h>

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

#define REFRESH_LIST 0x01
#define REFRESH_SORT 0x02
#define REFRESH_TAB 0x04

typedef unsigned char sidefx;

struct setmap {
    const char* name;
    size_t offset;
    void (*setfunc)(void* target, const char* setting, const void* params);
    sidefx fx;
    const void* params;
};

/* --Stuff for setting up the set mappings */

struct regcmd {
    const char* cmd;
    sidefx (*call)(const char**, void*);
    size_t expected_args;
    uint8_t lvl;
};

struct app_state;
size_t parse_toks(char* input, char** output, size_t ntoks);
const struct regcmd* get_cmd(const char* str);
void* lvl_pointer(uint8_t lvl, struct app_state* cfg);
sidefx call_cmd(const struct regcmd* cmd, const char** argv,
                struct app_state* cfg);
sidefx handle_cmd(char* cmd, struct app_state* cfg);


#endif
