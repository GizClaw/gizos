#include "h2/pal/h2_pal_unsupported.h"
#include "h2_c11_pal_atomic.h"

const h2_pal_atomic_api_t *h2_pal_unsupported_atomic_api(void) {
    return h2_c11_pal_atomic_api();
}
