#include "h2/pal/os/h2_pal_atomic_c11_impl.h"

static const h2_pal_atomic_vtable_t s_vtable = H2_C11_PAL_ATOMIC_VTABLE_INIT;
static const h2_pal_atomic_api_t s_api = { .user = NULL, .vtable = &s_vtable };

const h2_pal_atomic_api_t *h2_c11_pal_atomic_api(void) {
    return &s_api;
}
