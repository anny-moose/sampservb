#include <curses.h>
#include <signal.h>
#include <stddef.h>

sig_atomic_t child_spawned;

void getinput(const char* text, char* dest, size_t n) {
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
    if (stdscr == NULL || isendwin()) return;

    va_list(args);
    move(LINES - 2, 0);
    clrtoeol();
    va_start(args, format);
    vw_printw(stdscr, format, args);
    va_end(args);
    refresh();
}
