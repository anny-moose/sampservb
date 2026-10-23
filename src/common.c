#include "common.h"

#include <curses.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

sig_atomic_t child_spawned;
sig_atomic_t win_exists = 0;

void getinput(const char* text, char* dest, size_t n) {
    if (n == 0) return;
    if (win_exists == 0) {
        *dest = '\0';
        return;
    }

    move(LINES - 2, 0);
    clrtoeol();
    addnstr(text, COLS);
    curs_set(1);
    echo();
    getnstr(dest, n);
    noecho();
    curs_set(0);
}

void notify(const char* format, ...) {
    if (win_exists == 0) return;

    va_list(args);
    move(LINES - 2, 0);
    clrtoeol();
    va_start(args, format);
    vw_printw(stdscr, format, args);
    va_end(args);
    refresh();
}

void crash_exit(const char* format, ...) {
    if (win_exists != 0) {
        endwin();
        win_exists = 0;
    }

    va_list(args);
    va_start(args, format);

    vfprintf(stderr, format, args);

    va_end(args);
    fprintf(stderr, "\n");

    abort();
}
