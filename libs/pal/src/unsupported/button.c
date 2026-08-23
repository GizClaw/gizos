#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_button_read_single_button(void *p0, h2_pal_periph_id_t p1, h2_pal_single_button_reading_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_button_read_radio_button_group(void *p0, h2_pal_periph_id_t p1, h2_pal_radio_button_group_reading_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_button_vtable_t unsupported_button_vtable = {
    .read_single_button = unsupported_button_read_single_button,
    .read_radio_button_group = unsupported_button_read_radio_button_group,
};
static const h2_pal_button_api_t unsupported_button_api = { .user = NULL, .vtable = &unsupported_button_vtable };
const h2_pal_button_api_t *h2_pal_unsupported_button_api(void) { return &unsupported_button_api; }
