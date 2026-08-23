#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_periph_list(void *p0, h2_pal_periph_type_t p1, h2_pal_periph_cb_t p2, void *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_periph_get(void *p0, h2_pal_periph_id_t p1, h2_pal_periph_info_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_periph_vtable_t unsupported_periph_vtable = {
    .list = unsupported_periph_list,
    .get = unsupported_periph_get,
};
static const h2_pal_periph_api_t unsupported_periph_api = { .user = NULL, .vtable = &unsupported_periph_vtable };
const h2_pal_periph_api_t *h2_pal_unsupported_periph_api(void) { return &unsupported_periph_api; }
