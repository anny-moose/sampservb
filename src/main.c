
#define NCURSES_WIDECHAR 1
#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curses.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cmd.h"
#include "common.h"
#include "list.h"
#include "serv.h"
#include "servfetch.h"
#include "states.h"

int draw_tablist(WINDOW* win, const struct tab_state* tabs, size_t count,
                 size_t selected) {
    if (win == NULL || tabs == NULL) return -1;
    wmove(win, 0, 0);
    wclrtoeol(win);

    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            waddch(win, ' ');
            waddch(win, ' ');
        }
        if (selected == i) wattron(win, COLOR_PAIR(SEL_PAIR));
        wprintw(win, "%zu:%s", i + 1, tabs[i].tab_name);
        if (selected == i) wattroff(win, COLOR_PAIR(SEL_PAIR));
    }

    return 0;
}

void writestr_serv(WINDOW* win, const void* data, unsigned char col,
                   size_t elem, int xleft, void* userdata) {
    struct servlist* list = userdata;
    if (list == NULL) return;
    const struct servinfo* entry = data;
    entry += elem;

    short colattr = 0;
    wattr_get(win, NULL, &colattr, NULL);

    const char* txt = entry->txt == NULL ? list->txt : entry->txt;

    char buf[10];

    switch (col) {
        case 0:
            wcolor_set(win, entry->pa ? FAIL_PAIR : SUCC_PAIR, NULL);
            waddnwstr(win, entry->pa ? L"🔒" : L"🔓", xleft);
            break;
        case 1:
            waddnstr(win, txt + entry->hn_off, xleft);
            break;
        case 2:
            snprintf(buf, sizeof(buf), "%4" PRIu16 "/%-4." PRIu16, entry->pc,
                     entry->pm);
            waddnstr(win, buf, xleft);
            break;
        case 3:
            waddnstr(win, txt + entry->gm_off, xleft);
            break;
        case 4:
            waddnstr(win, txt + entry->ln_off, xleft);
            break;
        default:
            break;
    }

    wcolor_set(win, colattr, NULL);
}

unsigned char upd_listdesc(struct tab_state* tab, struct listdesc* ldesc) {
    ldesc->userdata = tab->list;

    ldesc->cols[0].width = tab->display.shown_fields & FIELD_PR ? 2 : 0;
    ldesc->cols[1].width =
        tab->display.shown_fields & FIELD_NAME ? tab->display.name_cols : 0;
    ldesc->cols[2].width = tab->display.shown_fields & FIELD_PC ? 9 : 0;
    ldesc->cols[3].width =
        tab->display.shown_fields & FIELD_GM ? tab->display.gm_cols : 0;
    ldesc->cols[4].width =
        tab->display.shown_fields & FIELD_LN ? tab->display.ln_cols : 0;

    unsigned char selcol = 0;
    int8_t currsort = tab->visual_sort;

    while ((currsort >>= 1) != 0) selcol++;

    return selcol;
}

void sigchld_handler(int sig) {
    if (sig != SIGCHLD) return;

    while (waitpid(-1, NULL, 0) < 0) {
        if (errno != EINTR) return;
    }

    child_spawned = 0;
    notify("Collected child game process");
}

int main(void) {
    struct sigaction handle_child = {
        .sa_flags = SA_RESTART,
        .sa_handler = sigchld_handler,
    };
    if (sigaction(SIGCHLD, &handle_child, NULL) < 0)
        err(EXIT_FAILURE, "Failed to set signal handler for SIGCHLD");
    handle_child.sa_flags = 0;
    if (sigaction(SIGALRM, &handle_child, NULL) < 0)
        err(EXIT_FAILURE, "Failed to set signal handler for SIGALRM");

    setlocale(LC_ALL, "");
    initscr();

    start_color();
    init_pair(SUCC_PAIR, COLOR_YELLOW, COLOR_GREEN);
    init_pair(FAIL_PAIR, COLOR_YELLOW, COLOR_RED);
    init_pair(SEL_PAIR, COLOR_BLACK, COLOR_WHITE);

    cbreak();
    noecho();
    noqiflush();
    curs_set(0);
    keypad(stdscr, true);

    refresh();

    WINDOW* listwin = newwin(LINES - 2, COLS, 0, 0);
    WINDOW* tabwin = newwin(1, COLS, LINES - 1, 0);

    servquery_init();

    struct tab_state* t = malloc(sizeof(struct tab_state));
    struct app_state state = {
        .tabs = t,
        .tabs_count = 1,
        .tabs_capacity = 1,
        .servlist_win = listwin,
    };

    init_tab(state.tabs);
    struct tab_state* tab = state.tabs + state.tabs_selected;

    struct coldesc cols[5] = {
        {"Password", 2},
        {"Name", state.tabs->display.name_cols},
        {"Players", 9},
        {"Gamemode", state.tabs->display.gm_cols},
        {"Language", state.tabs->display.ln_cols},
    };

    struct listdesc listd = {
        .display_name = "Servers",
        .ncols = 5,
        .cols = cols,
        .writestr = writestr_serv,
        .opt = LIST_OPT_SHOWSEL | LIST_OPT_SHOWSELCOL,
    };

    listd.userdata = NULL;
    draw_list(listwin, &listd, NULL, 0, tab->selected,
              upd_listdesc(tab, &listd));
    wrefresh(listwin);

    char buf[64] = {0};
    sidefx fx;
    while (state.quit == false) {
        int ch = getch();
        tab = state.tabs + state.tabs_selected;

        size_t idx;

        switch (ch) {
            case '\n':
                idx = ':' - 32;
                break;
            case '\t':
                idx = '/' - 32;
                break;
            default:
                idx = ch - 32;
        }

        if (idx >= 95) {
            continue;
        }

        /* I don't particularly like this approach either to be honest, but it's
         * ever so slightly less static than the last one. */
        if (ch == ':') {
            getinput(":", buf, 63);
            fx = handle_cmd(buf, &state);
        } else if (ch == '/') {
            getinput("Enter search request: ", tab->search_buf, 63);
            if (tab->list->num_displayed < tab->selected)
                tab->selected = tab->list->num_displayed - 1;
            fx = REFRESH_SORT | REFRESH_LIST;
        } else {
            fx = keymapping_call(state.keycmd[idx], &state);
        }

        if (fx & REFRESH_TAB) {
            tab = state.tabs + state.tabs_selected;
            draw_tablist(tabwin, state.tabs, state.tabs_count,
                         state.tabs_selected);
            wrefresh(tabwin);
        }
        if (fx & REFRESH_SORT)
            sort_serverlist(tab->list, tab->sort, tab->filters,
                            tab->search_buf);
        if (fx & REFRESH_LIST) {
            draw_list(listwin, &listd, tab->list->servs,
                      tab->list->num_displayed, tab->selected,
                      upd_listdesc(tab, &listd));
            wrefresh(listwin);
        }

        fx = 0;
    }

    servquery_destroy();

    for (size_t i = 0; i < state.tabs_count; i++) free_tab(state.tabs + i);
    free(state.tabs);

    for (size_t i = 0; i < sizeof(state.keycmd) / sizeof(state.keycmd[0]); i++)
        free(state.keycmd[i]);

    delwin(tabwin);
    delwin(listwin);
    endwin();

    return EXIT_SUCCESS;
}
