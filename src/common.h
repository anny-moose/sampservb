#ifndef COMMON_H_
#define COMMON_H_
#include <signal.h>
#include <stddef.h>

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

#define SUCC_PAIR 1
#define FAIL_PAIR 2
#define SEL_PAIR 3

extern sig_atomic_t child_spawned;
extern sig_atomic_t win_exists;

void getinput(const char* text, char* dest, size_t n);
void notify(const char* format, ...);

void crash_exit(const char* format, ...);

#endif
