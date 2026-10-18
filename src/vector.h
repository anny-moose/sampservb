/*
 * Set of functions for managing dynamic arrays, meant to be used with other
 * data types that follow a similar declarations, using the vector structure as
 * a temporary proxy.
 */
#include <stddef.h>

struct vector {
    void* data;
    size_t len;
    size_t cap;
    size_t ds;
};

int vector_init(struct vector* vec, const size_t cap, const size_t data_size);
int vector_push(struct vector* vec, const void* src);
int vector_resize(struct vector* vec, const size_t new_cap);
int vector_swap_pop(struct vector* vec, const size_t idx, void* out);
void vector_destroy(struct vector* vec);
