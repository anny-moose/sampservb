#include "list.h"

#include <curses.h>
#include <stddef.h>

#include "common.h"

void draw_list(WINDOW* win, struct listdesc* desc, const void* data,
               size_t num_elements, size_t sel, size_t selcol) {
    if (win == NULL || desc == NULL) return;

    werase(win);

    int maxy, maxx, y, x, minx;
    getmaxyx(win, maxy, maxx);
    y = 0;
    x = 0;
    minx = 0;
    if (desc->opt & LIST_OPT_BORDER) {
        box(win, 0, 0);
        minx = 1;
        maxx--;
        maxy--;
    }

    wmove(win, 0, minx);

    if (desc->display_name != NULL) {
        waddnstr(win, desc->display_name, maxx);
        y++;
        wmove(win, y, x);
    }

    int xleft;
    for (unsigned char col = 0; col < desc->ncols; col++) {
        if (x >= maxx) break;
        xleft = maxx - x;
        struct coldesc* colp = desc->cols + col;
        int xinc = MIN(colp->width, xleft);
        if (xinc == 0) continue;

        if (col == selcol && desc->opt & LIST_OPT_SHOWSELCOL)
            wcolor_set(win, SEL_PAIR, NULL);
        waddnstr(win, colp->name, xinc);
        wcolor_set(win, 0, NULL);

        x += xinc + 2;
        if (wmove(win, y, x) == ERR) break;
    }

    y++;
    x = minx;
    wmove(win, y, x);

    if (desc->opt & LIST_OPT_SELSCR)
        desc->begin = sel;
    else {
        int yleft = maxy - y;
        if (desc->begin > sel)
            desc->begin = sel;
        else if (sel - desc->begin >= (size_t)yleft) {
            desc->begin = sel - yleft + 1;
        }
    }
    size_t i = desc->begin;

    if (desc->opt & LIST_OPT_BORDER) {
        if (i > 0) {
            wmove(win, y, maxx);
            waddch(win, '^');
        }
        if (i + (maxy - y) < num_elements) {
            wmove(win, maxy - 1, maxx);
            waddch(win, 'v');
        }
    }

    for (; y < maxy; y++) {
        x = minx;
        wmove(win, y, x);

        if (i >= num_elements) break;

        if (sel == i && desc->opt & LIST_OPT_SHOWSEL)
            wcolor_set(win, SEL_PAIR, NULL);

        for (unsigned char col = 0; col < desc->ncols; col++) {
            if (x >= maxx) break;
            xleft = maxx - x;
            int xinc = MIN(desc->cols[col].width, xleft);
            if (xinc == 0) continue;

            desc->writestr(win, data, col, i, xinc, desc->userdata);

            x += xinc + 2;
            if (wmove(win, y, x) == ERR) break;
        }

        wcolor_set(win, 0, NULL);

        i++;
    }
}
