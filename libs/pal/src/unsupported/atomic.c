#include "h2/pal/h2_pal_unsupported.h"
#include "h2/pal/os/h2_pal_atomic_c11_impl.h"

static const h2_pal_atomic_vtable_t s_atomic_vtable =
    H2_C11_PAL_ATOMIC_VTABLE_INIT;
static const h2_pal_atomic_api_t s_atomic_api = {
    .user = NULL, .vtable = &s_atomic_vtable,
};

const h2_pal_atomic_api_t *h2_pal_unsupported_atomic_api(void) {
    return &s_atomic_api;
}
