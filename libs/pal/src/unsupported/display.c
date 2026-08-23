#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_display_open(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_display_get_info(void *p0, h2_display_info_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_display_draw_bitmap(void *p0, const h2_display_rect_t *p1, const void *p2, size_t p3, h2_display_pixel_format_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_display_present(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_display_set_brightness_percent(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_display_close(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_display_vtable_t unsupported_display_vtable = {
    .open = unsupported_display_open,
    .get_info = unsupported_display_get_info,
    .draw_bitmap = unsupported_display_draw_bitmap,
    .present = unsupported_display_present,
    .set_brightness_percent = unsupported_display_set_brightness_percent,
    .close = unsupported_display_close,
};
static const h2_pal_display_api_t unsupported_display_api = { .user = NULL, .vtable = &unsupported_display_vtable };
const h2_pal_display_api_t *h2_pal_unsupported_display_api(void) { return &unsupported_display_api; }
