#ifndef COMMON_H_
#define COMMON_H_
#include <signal.h>
#include <stddef.h>

#define MIN(x, y) (x) < (y) ? (x) : (y)
#define MAX(x, y) (x) > (y) ? (x) : (y)

extern sig_atomic_t child_spawned;

void getinput(const char* text, char* dest, size_t n);
void notify(const char* format, ...);

#endif
