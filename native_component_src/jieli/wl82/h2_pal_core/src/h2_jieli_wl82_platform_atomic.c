#include "h2/pal/os/h2_pal_atomic.h"
#include <stdlib.h>

/* malloc supports the alignment of every fundamental C type. */
typedef union { void *pointer; long double floating; long long integer; }
    h2_atomic_malloc_alignment_t;

/* PSRAM and multicore semantics have not yet been verified on hardware. */
static h2_pal_result_t atomic_alloc(void *user, size_t size,
                                     size_t alignment, void **out) {
    (void)user;
    if (out == NULL || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0)
        return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if (alignment > _Alignof(h2_atomic_malloc_alignment_t)) return H2_PAL_ERR_UNSUPPORTED;
    *out = malloc(size);
    return *out == NULL ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}

static void atomic_free(void *user, void *value) {
    (void)user;
    free(value);
}

static const h2_pal_atomic_vtable_t s_vtable = {
    .alloc = atomic_alloc, .free = atomic_free,
};
static const h2_pal_atomic_api_t s_api = { .user = NULL, .vtable = &s_vtable };

const h2_pal_atomic_api_t *h2_jieli_wl82_platform_atomic_api(void) {
    return &s_api;
}
