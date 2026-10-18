#include "vector.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* further calls presume that vec->cap * data_size will always be valid and that
 * len is < cap */
int vector_init(struct vector* vec, const size_t cap, const size_t data_size) {
    if (vec == NULL || data_size == 0) return -EINVAL;

    if (cap != 0) {
        if (SIZE_MAX / cap < data_size) return -EOVERFLOW;
        vec->data = malloc(cap * data_size);
        if (vec->data == NULL) return -errno;
    } else {
        vec->data = NULL;
    }

    vec->cap = cap;
    vec->len = 0;
    vec->ds = data_size;

    return 0;
}

int vector_resize(struct vector* vec, const size_t new_cap) {
    if (SIZE_MAX / new_cap < vec->ds) return -EOVERFLOW;
    if (vec->len > new_cap) return -EINVAL;

    void* data = realloc(vec->data, new_cap * vec->ds);
    if (data == NULL) return -errno;
    vec->data = data;
    vec->cap = new_cap;

    return 0;
}

int vector_push(struct vector* vec, const void* src) {
    if (vec == NULL || src == NULL) return -EINVAL;

    if (vec->len >= vec->cap) {
        if (SIZE_MAX - vec->cap < vec->cap / 2) return -EOVERFLOW;
        size_t new_cap = (vec->cap < 2) ? 2 : vec->cap + vec->cap / 2;

        int ret = vector_resize(vec, new_cap);
        if (ret < 0) return ret;
    }

    size_t end = vec->len * vec->ds;

    memcpy((char*)vec->data + end, src, vec->ds);
    vec->len++;
    return 0;
}

int vector_swap_pop(struct vector* vec, const size_t idx, void* out) {
    if (vec == NULL) return -EINVAL;
    if (idx >= vec->len) return -ENOENT;

    size_t elem = idx * vec->ds;
    size_t last = (vec->len - 1) * vec->ds;

    char* data = vec->data;
    if (out) memcpy(out, data + elem, vec->ds);

    if (elem != last) {
        memcpy(data + elem, data + last, vec->ds);
    }

    vec->len--;
    return 0;
}

void vector_destroy(struct vector* vec) {
    if (vec == NULL) return;

    free(vec->data);
    memset(vec, 0, sizeof(struct vector));
}
