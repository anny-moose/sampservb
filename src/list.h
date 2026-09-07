#ifndef LIST_H_
#define LIST_H_

#include <curses.h>
#include <stddef.h>

#define LIST_OPT_SHOWSEL 0x01
#define LIST_OPT_SHOWSELCOL 0x02
#define LIST_OPT_SELSCR 0x04
#define LIST_OPT_BORDER 0x08

struct listdesc {
    unsigned char ncols;
    const char* display_name;
    struct coldesc {
        const char* name;
        unsigned char width;
    }* cols;

    size_t begin;
    void (*writestr)(WINDOW* win, const void* data, unsigned char col,
                     size_t elem, int xleft, void* userdata);

    void* userdata;
    uint8_t opt; /* e.g display sel/selcol, treat sel as  */
};

void draw_list(WINDOW* win, struct listdesc* desc, const void* data,
               size_t num_elements, size_t sel, size_t selcol);

#endif
