#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_led_get_info(void *p0, h2_pal_led_id_t p1, h2_pal_led_info_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_led_set_frame(void *p0, h2_pal_led_id_t p1, const h2_pal_led_color_t *p2, size_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_led_set_solid(void *p0, h2_pal_led_id_t p1, h2_pal_led_color_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_led_clear(void *p0, h2_pal_led_id_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_led_set_brightness_percent(void *p0, h2_pal_led_id_t p1, uint8_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_led_vtable_t unsupported_led_vtable = {
    .get_info = unsupported_led_get_info,
    .set_frame = unsupported_led_set_frame,
    .set_solid = unsupported_led_set_solid,
    .clear = unsupported_led_clear,
    .set_brightness_percent = unsupported_led_set_brightness_percent,
};
static const h2_pal_led_api_t unsupported_led_api = { .user = NULL, .vtable = &unsupported_led_vtable };
const h2_pal_led_api_t *h2_pal_unsupported_led_api(void) { return &unsupported_led_api; }
