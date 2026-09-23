#include "h2/pal/os/h2_pal_atomic_malloc_impl.h"

/* PSRAM and multicore semantics have not yet been verified on hardware. */
static const h2_pal_atomic_vtable_t s_vtable = H2_PAL_ATOMIC_MALLOC_VTABLE_INIT;
static const h2_pal_atomic_api_t s_api = { .user = NULL, .vtable = &s_vtable };

const h2_pal_atomic_api_t *h2_bk_platform_atomic_api(void) {
    return &s_api;
}
