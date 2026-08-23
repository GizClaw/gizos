#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_input_read_motion(void *p0, h2_pal_periph_id_t p1, h2_pal_motion_reading_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_input_read_battery(void *p0, h2_pal_periph_id_t p1, h2_pal_battery_reading_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_input_read_temperature(void *p0, h2_pal_periph_id_t p1, h2_pal_temperature_reading_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_input_vtable_t unsupported_input_vtable = {
    .read_motion = unsupported_input_read_motion,
    .read_battery = unsupported_input_read_battery,
    .read_temperature = unsupported_input_read_temperature,
};
static const h2_pal_input_api_t unsupported_input_api = { .user = NULL, .vtable = &unsupported_input_vtable };
const h2_pal_input_api_t *h2_pal_unsupported_input_api(void) { return &unsupported_input_api; }
