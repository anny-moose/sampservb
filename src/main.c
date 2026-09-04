
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
#include "serv.h"
#include "servfetch.h"
#include "states.h"

#define SUCC_PAIR 1
#define FAIL_PAIR 2
#define SEL_PAIR 3

#define TEST_FIELD_OPEN(bit)                                   \
    do {                                                       \
        if (field & (bit)) wattron(win, COLOR_PAIR(SEL_PAIR)); \
        if (cfg.shown_fields & (bit) && ok != ERR)

#define TEST_FIELD_CLOSE(bit)                               \
    if (field & (bit)) wattroff(win, COLOR_PAIR(SEL_PAIR)); \
    }                                                       \
    while (0)

int draw_frame(WINDOW* win, struct display_cfg cfg, int8_t field) {
    int maxy, maxx, y, x;
    (void)(maxy);
    getmaxyx(win, maxy, maxx);
    y = 0;
    x = 0;
    wclear(win);
    wmove(win, 0, 0);

    int ok = OK;
    TEST_FIELD_OPEN(FIELD_PR) {
        waddch(win, 'P');
        x += 4;
        ok = wmove(win, y, x);
    }
    TEST_FIELD_CLOSE(FIELD_PR);
    TEST_FIELD_OPEN(FIELD_NAME) {
        waddstr(win, "Name");
        x += cfg.name_cols + 2;
        ok = wmove(win, y, x);
    }
    TEST_FIELD_CLOSE(FIELD_NAME);
    TEST_FIELD_OPEN(FIELD_PC) {
        waddstr(win, "Players");
        x += 11;
        ok = wmove(win, y, x);
    }
    TEST_FIELD_CLOSE(FIELD_PC);
    TEST_FIELD_OPEN(FIELD_GM) {
        waddstr(win, "Gamemode");
        x += cfg.gm_cols + 2;
        ok = wmove(win, y, x);
    }
    TEST_FIELD_CLOSE(FIELD_GM);
    TEST_FIELD_OPEN(FIELD_LN) {
        waddstr(win, "Language");
        x += cfg.ln_cols + 2;
        ok = wmove(win, y, x);
    }
    TEST_FIELD_CLOSE(FIELD_LN);

    wrefresh(win);

    return 0;
}

int draw_serverlist(WINDOW* win, const struct servlist* servers,
                    struct display_cfg cfg, size_t selected) {
    if (win == NULL) return -1;
    if (servers == NULL) {
        wclear(win);
        return -1;
    }
    static size_t beginning = 0;
    if (beginning > selected) beginning = selected;

    int maxy, maxx, y, x;
    getmaxyx(win, maxy, maxx);
    y = 0;
    x = 0;
    wmove(win, 0, 0);

    if (selected - beginning >= (unsigned)maxy) beginning = selected - maxy + 1;

    for (y = 0; y < maxy; y++) {
        x = 0;
        wmove(win, y, x);
        wclrtoeol(win);

        size_t i = beginning + y;
        if (i >= servers->num_displayed) continue;

        const char* txt = servers->servs[i].txt == NULL ? servers->txt
                                                        : servers->servs[i].txt;

        int ok = OK;
        if (cfg.shown_fields & FIELD_PR && ok != ERR) {
            short col = servers->servs[i].pa ? FAIL_PAIR : SUCC_PAIR;

            wattron(win, COLOR_PAIR(col));
            waddwstr(win, servers->servs[i].pa ? L"🔒" : L"🔓");
            wattroff(win, COLOR_PAIR(col));
            x += 4;
            ok = wmove(win, y, x);
        }
        if (selected == i) wattron(win, COLOR_PAIR(SEL_PAIR));
        if (cfg.shown_fields & FIELD_NAME && ok != ERR) {
            waddnstr(win, txt + servers->servs[i].hn_off, cfg.name_cols);
            x += cfg.name_cols + 2;
            ok = wmove(win, y, x);
        }
        if (cfg.shown_fields & FIELD_PC && ok != ERR) {
            wprintw(win, "%4" PRIu16 "/%-4." PRIu16, servers->servs[i].pc,
                    servers->servs[i].pm);
            x += 11;
            ok = wmove(win, y, x);
        }
        if (cfg.shown_fields & FIELD_GM && ok != ERR) {
            waddnstr(win, txt + servers->servs[i].gm_off, cfg.gm_cols);
            x += cfg.gm_cols + 2;
            ok = wmove(win, y, x);
        }
        if (cfg.shown_fields & FIELD_LN && ok != ERR) {
            waddnstr(win, txt + servers->servs[i].ln_off, cfg.ln_cols);
            x += cfg.ln_cols + 2;
            ok = wmove(win, y, x);
        }
        if (selected == i) wattroff(win, COLOR_PAIR(SEL_PAIR));
    }

    return 0;
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

    WINDOW* status = newwin(LINES - 1 - 2, COLS, 1, 0);
    WINDOW* sortwin = newwin(1, COLS, 0, 0);

    servquery_init();

    struct tab_state* t = malloc(sizeof(struct tab_state));
    struct app_state state = {
        .tabs = t,
        .tabs_count = 1,
        .tabs_capacity = 1,
        .servlist_win = status,
    };

    init_tab(state.tabs);
    struct tab_state* tab = state.tabs + state.tabs_selected;

    draw_serverlist(status, tab->list, tab->display, 0);
    wrefresh(status);
    draw_frame(sortwin, tab->display, 0);

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

        if (fx & REFRESH_TAB) tab = state.tabs + state.tabs_selected;
        if (fx & REFRESH_SORT)
            sort_serverlist(tab->list, tab->sort, tab->filters,
                            tab->search_buf);
        if (fx & REFRESH_LIST) {
            draw_serverlist(status, tab->list, tab->display, tab->selected);
            wrefresh(status);
        }

        draw_frame(sortwin, tab->display, tab->visual_sort);
        fx = 0;
    }

    servquery_destroy();

    for (size_t i = 0; i < state.tabs_count; i++) free_tab(state.tabs + i);
    free(state.tabs);

    for (size_t i = 0; i < sizeof(state.keycmd) / sizeof(state.keycmd[0]); i++)
        free(state.keycmd[i]);

    delwin(status);
    delwin(sortwin);
    endwin();

    return EXIT_SUCCESS;
}
