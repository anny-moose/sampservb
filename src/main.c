
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
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cmd.h"
#include "common.h"
#include "keymap.h"
#include "list.h"
#include "serv.h"
#include "servfetch.h"
#include "states.h"

int draw_tablist(WINDOW* win, const struct rc_tab* tabs, size_t count,
                 size_t selected) {
    if (win == NULL || tabs == NULL) return -1;
    wmove(win, 0, 0);
    wclrtoeol(win);

    size_t shown = 0;
    for (size_t i = 0; i < count; i++) {
        const struct rc_tab* tab = tabs + i;
        if (tab->refs == 0) continue;

        if (shown > 0) {
            waddch(win, ' ');
            waddch(win, ' ');
        }
        if (selected == i) wattron(win, COLOR_PAIR(SEL_PAIR));
        wprintw(win, "%zu:%s", i + 1, tab->tab.tab_name);
        if (selected == i) wattroff(win, COLOR_PAIR(SEL_PAIR));
        shown++;
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

static sidefx handle_input(struct app_state* state);
static sidefx handle_socket(struct app_state* state);
static void handle_sidefx(struct app_state* state, struct rc_tab** tab_,
                          sidefx fx, WINDOW* tabwin, WINDOW* listwin);
static sidefx handle_expired(struct app_state* state, size_t searchstart);

int main(void) {
    struct sigaction handle_child = {
        .sa_flags = SA_RESTART,
        .sa_handler = sigchld_handler,
    };
    if (sigaction(SIGCHLD, &handle_child, NULL) < 0)
        err(EXIT_FAILURE, "Failed to set signal handler for SIGCHLD");

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

    struct app_state state = {0};

    int ret = packet_list_init(&state.packs);
    if (ret < 0) {
        errx(EXIT_FAILURE, "Failed to initialize packet list: %s",
             strerror(-ret));
    }

    tabs_reallocate(&state.tabs, 4);

    ret = tabs_newtab(&state.tabs, NULL, 0);
    if (ret < 0) {
        errx(EXIT_FAILURE, "Failed to create new tab: %s", strerror(-ret));
    }

    struct rc_tab* tab = app_getcurrtab(&state);

    handle_sidefx(&state, &tab, ~0, tabwin, listwin);
    wrefresh(tabwin);
    wrefresh(listwin);

    keymap_init(&state.keymap, 16);

    int sockfd = servquery_getfd();
    if (sockfd < 0)
        err(EXIT_FAILURE, "Failed to get socket fd: %s", strerror(-sockfd));

    struct pollfd fds[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = sockfd, .events = POLLIN},
    };

    sidefx fx;
    int wtime = -1;
    while (state.quit == false) {
        tab = app_getcurrtab(&state);

        ret = poll(fds, 2, wtime);
        if (ret < 0) {
            if (errno == EINTR) continue;
            err(EXIT_FAILURE, "poll failed");
        }

        if (fds[0].revents & POLLERR || fds[0].revents & POLLNVAL)
            errx(EXIT_FAILURE, "An error happened on stdin");

        if (fds[1].revents & POLLERR || fds[1].revents & POLLNVAL)
            errx(EXIT_FAILURE, "An error happened on socket");

        if (fds[0].revents & POLLIN) {
            fx = handle_input(&state);
            handle_sidefx(&state, &tab, fx, tabwin, listwin);
        }

        if (fds[1].revents & POLLIN) {
            fx = 0;
            do {
                fx |= handle_socket(&state);
            } while (servquery_respavail());
            handle_sidefx(&state, &tab, fx, tabwin, listwin);
        }

        size_t expi;
        packet_list_passtime(&state.packs, &expi);
        fx = handle_expired(&state, expi);
        handle_sidefx(&state, &tab, fx, tabwin, listwin);

        wtime = packet_list_getmintimeout_ms(&state.packs);

        wrefresh(tabwin);
        wrefresh(listwin);
    }

    ret = packet_list_shutdown_sequence(&state.packs);

    delwin(tabwin);
    delwin(listwin);
    endwin();

    if (ret < 0) {
        warnx("Failed to cleanly destroy packet list???");
    }

    ret = packet_list_destroy(&state.packs);
    if (ret < 0) {
        warnx("Failed to destroy packet list: %s", strerror(-ret));
        if (state.packs.len > 0) {
            warnx("Leaked packet list entries amount >0! (%zu)",
                  state.packs.len);
        }
    }

    ret = servquery_destroy();
    if (ret < 0) {
        warnx("Failed to destroy servquery mechanism: %s", strerror(-ret));
    }

    for (size_t i = 0; i < state.tabs.cap; i++) {
        if (state.tabs.tabs[i].refs > 0) tab_post(&state.tabs.tabs[i]);
        if (state.tabs.tabs[i].refs > 0) {
            warnx("A tab wasn't freed on exit! # refs left: %ju",
                  (uintmax_t)state.tabs.tabs[i].refs);
        }
    }

    free(state.tabs.tabs);
    free(state.tabs.inds);

    keymap_destroy(&state.keymap);

    return EXIT_SUCCESS;
}

#define INPUT_SIZE 64
sidefx handle_input(struct app_state* state) {
    char buf[INPUT_SIZE];
    sidefx fx = 0;

    int ch = getch();
    if (ch == ':') {
        getinput(":", buf, INPUT_SIZE);
        fx = handle_cmd(buf, state);
    } else if (ch == '/') {
        struct rc_tab* tab = app_getcurrtab(state);
        if (tab == NULL)
            return 0;
        else if (tab->tab.list == NULL) {
            notify("Can't search on an empty list");
            return 0;
        }
        getinput("Enter search request: ", tab->tab.search_buf, INPUT_SIZE);
        if (tab->tab.list->num_displayed < tab->tab.selected)
            tab->tab.selected = tab->tab.list->num_displayed - 1;
        fx = REFRESH_SORT | REFRESH_LIST;
    } else {
        fx = keymap_call(&state->keymap, ch, state);
    }

    return fx;
}

sidefx handle_socket(struct app_state* state) {
    union servquery_out packet;
    bool consumed;
    struct sockaddr_in origin;
    struct expected_packet pack;

    int ret = servquery_inspectresp(&origin);
    if (ret < 0) {
        if (ret == -EBADE) servquery_discardresp();
        notify("Error while handling socket: %s", strerror(-ret));
        return 0;
    }

    ret = packet_list_popmatch(&state->packs, origin, ret, &pack);
    if (ret < 0) {
        if (ret == -ENOENT) {
            notify("Unexpected incoming packet");
            servquery_discardresp();
            return 0;
        } else {
            notify("Error while processing packet: %s (%d)", strerror(-ret),
                   -ret);
            return 0;
        }
    }

    ret = servquery_getresp(&packet, NULL, NULL, SERVQUERY_OPCODE_UNKNOWN,
                            pack.custom_txt ? pack.txt.txt_p : &pack.txt.txt,
                            &consumed);
    if (ret < 0) {
        if (consumed == false) servquery_discardresp();
        notify("Error while processing packet: %s (%d)", strerror(-ret), -ret);
        return 0;
    }

    if (ret < 0) {
        notify("Error while getting server response: %s", strerror(-ret));
        if (consumed == false && -ret != EINTR) servquery_discardresp();
        return 0;
    }

    if (pack.act == NULL) return 0;
    return pack.act(&packet, &pack, &state->packs);
}

static void handle_sidefx(struct app_state* state, struct rc_tab** tab_,
                          sidefx fx, WINDOW* tabwin, WINDOW* listwin) {
    static struct coldesc cols[5] = {
        {"Password", 2}, {"Name", 0},     {"Players", 9},
        {"Gamemode", 0}, {"Language", 0},
    };

    static char display_name[] = "Servers (xxxx)";

    static struct listdesc listd = {
        .display_name = display_name,
        .ncols = 5,
        .cols = cols,
        .writestr = writestr_serv,
        .opt = LIST_OPT_SHOWSEL | LIST_OPT_SHOWSELCOL,
    };

    struct rc_tab* tab = *tab_;

    if (fx & REFRESH_TAB) {
        tab = app_getcurrtab(state);
        if (tabwin != NULL)
            draw_tablist(tabwin, state->tabs.tabs, state->tabs.cap,
                         state->tabs.sel);
    }

    if (fx & REFRESH_SORT) {
        if (tab != NULL)
            sort_serverlist(tab->tab.list, tab->tab.sort, tab->tab.filters,
                            tab->tab.search_buf);
    }

    if (fx & REFRESH_LIST) {
        if (listwin != NULL) {
            size_t servcnt = 0;

            if (tab != NULL) {
                if (tab->tab.list != NULL) {
                    servcnt = MIN(9999, tab->tab.list->num_displayed);
                }
            }

            snprintf(display_name, sizeof(display_name), "Servers (%zu)",
                     servcnt);

            if (tab == NULL) {
                draw_list(listwin, &listd, NULL, 0, 0, 0);
            } else {
                draw_list(
                    listwin, &listd,
                    tab->tab.list == NULL ? NULL : tab->tab.list->servs,
                    tab->tab.list == NULL ? 0 : tab->tab.list->num_displayed,
                    tab->tab.selected, upd_listdesc(&tab->tab, &listd));
            }
        }
    }

    *tab_ = tab;
}

static sidefx handle_expired(struct app_state* state, size_t searchstart) {
    struct expected_packet pack;
    sidefx ret = 0;

    while (searchstart != state->packs.len) {
        int err = packet_list_popexpired(&state->packs, &searchstart, &pack);
        if (err < 0) {
            if (err == -ENOENT) break;
            notify("Error while going through expired packets");
            break;
        }

        if (pack.tact == NULL) continue;
        ret |= pack.tact(&pack, &state->packs);
    }

    return ret;
}
