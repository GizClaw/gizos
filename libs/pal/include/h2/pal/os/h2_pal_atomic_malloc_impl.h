#ifndef H2_PAL_ATOMIC_MALLOC_IMPL_H
#define H2_PAL_ATOMIC_MALLOC_IMPL_H

#include "h2/pal/os/h2_pal_atomic.h"
#include <stdlib.h>
#include <stddef.h>

/* Generic provider for platforms whose ordinary heap is suitable for C11
 * atomic storage. The caller is responsible for the platform's memory policy. */
static inline h2_pal_result_t h2_pal_atomic_malloc_alloc(
    void *user, size_t size, size_t alignment, void **out) {
    (void)user;
    if (out == NULL || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0)
        return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if (alignment > _Alignof(void *)) return H2_PAL_ERR_UNSUPPORTED;
    *out = malloc(size);
    return *out == NULL ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}
static inline void h2_pal_atomic_malloc_free(void *user, void *value) {
    (void)user;
    free(value);
}
#define H2_PAL_ATOMIC_MALLOC_VTABLE_INIT \
    { .alloc = h2_pal_atomic_malloc_alloc, .free = h2_pal_atomic_malloc_free }
#endif
