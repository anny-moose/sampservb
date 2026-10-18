#ifndef STATES_H_
#define STATES_H_

#include <curses.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>

#include "cmd.h"
#include "keymap.h"
#include "servfetch.h"

#define FIELD_PR 0x1
#define FIELD_NAME 0x2
#define FIELD_PC 0x4
#define FIELD_GM 0x8
#define FIELD_LN 0x10

struct display_cfg {
    uint8_t shown_fields;
    uint8_t name_cols;
    uint8_t gm_cols;
    uint8_t ln_cols;
};

#define HIDE_PR 0x1
#define HIDE_EMPTY 0x2
#define HIDE_FULL 0x4

#define TABNAME_LEN 25
struct tab_state {
    char tab_name[TABNAME_LEN];
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

#define TAB_MAXREFS UINT8_MAX

struct rc_tab {
    struct tab_state tab;
    uint8_t refs;  /* 255 is more than enough */
    bool markfree; /* for potentially skipping operations in functions that
                      still reference the tab */
};

#define TABS_STARTCNT 4

struct packet_list;

/* commands that query servers using servquery_* functions are expected add
 * their expected packet and action function to the expected_packet list in
 * app_state, rather than wait for the packet synchronously. */
struct expected_packet {
    struct timeval timeout;
    struct sockaddr_in from;
    enum servquery_opcode op; /* UNKNOWN not allowed. */

    bool custom_txt;
    union {
        struct servquery_textbuf txt;
        struct servquery_textbuf* txt_p;
    } txt;

    void* state;

    sidefx (*act)(union servquery_out* pack, struct expected_packet* self,
                  struct packet_list* list);
    sidefx (*tact)(struct expected_packet* self, struct packet_list* list);
};

struct packet_list {
    struct timeval prevtime;
    struct expected_packet* packets;
    size_t len;
    size_t cap;
    bool accepting;
};

int packet_list_init(struct packet_list* list);

int packet_list_addpacket(
    struct packet_list* list,
    struct expected_packet* packet); /* implicitly invokes passtime */
int packet_list_getmintimeout_ms(
    const struct packet_list* list); /* specifically for poll */

/* optionally writes the index of the first expired event into firsti */
int packet_list_passtime(struct packet_list* list, size_t* firsti);

int packet_list_popmatch(struct packet_list* list, struct sockaddr_in from,
                         enum servquery_opcode op, struct expected_packet* out);

int packet_list_popexpired(struct packet_list* list, size_t* startidx,
                           struct expected_packet* out);

int packet_list_shutdown_sequence(struct packet_list* list);

int packet_list_destroy(struct packet_list* list);

struct app_state {
    bool quit;
    pid_t game_proc;

    struct keymap keymap;

    struct tabs {
        struct rc_tab* tabs;
        size_t* inds; /* indices for e.g rearranging tabs */
        size_t cap;
        size_t sel;
    } tabs;

    struct packet_list packs;

    /* WINDOW* const servlist_win; */ /* leftover?? */
};

struct tab_state* app_getcurrtab_raw(struct app_state* state);
struct rc_tab* app_getcurrtab(struct app_state* state);
size_t app_getnexttab(struct app_state* state);
size_t app_getprevtab(struct app_state* state);

int tab_init(struct rc_tab* out);
int tab_wait(struct rc_tab* tab);
void tab_post(struct rc_tab* target);
int tabs_reallocate(struct tabs* tabs, size_t new_size);
int tabs_newtab(struct tabs* tabs, const char* name, size_t nlen);

#endif
