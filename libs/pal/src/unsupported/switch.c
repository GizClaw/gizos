#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_switch_set(void *p0, h2_pal_periph_id_t p1, h2_pal_switch_state_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_switch_get(void *p0, h2_pal_periph_id_t p1, h2_pal_switch_state_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_switch_vtable_t unsupported_switch_vtable = {
    .set = unsupported_switch_set,
    .get = unsupported_switch_get,
};
static const h2_pal_switch_api_t unsupported_switch_api = { .user = NULL, .vtable = &unsupported_switch_vtable };
const h2_pal_switch_api_t *h2_pal_unsupported_switch_api(void) { return &unsupported_switch_api; }
