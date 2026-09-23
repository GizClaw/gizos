#include "h2/pal/h2_pal_unsupported.h"

static h2_pal_result_t atomic_alloc(void *user, size_t size,
                                     size_t alignment, void **out) {
    (void)user;
    (void)size;
    (void)alignment;
    if (out != NULL) *out = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void atomic_free(void *user, void *value) {
    (void)user;
    (void)value;
}

static const h2_pal_atomic_vtable_t s_vtable = {
    .alloc = atomic_alloc, .free = atomic_free,
};
static const h2_pal_atomic_api_t s_api = { .user = NULL, .vtable = &s_vtable };

const h2_pal_atomic_api_t *h2_pal_unsupported_atomic_api(void) {
    return &s_api;
}
