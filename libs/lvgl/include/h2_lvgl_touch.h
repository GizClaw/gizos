#ifndef H2_LVGL_TOUCH_H
#define H2_LVGL_TOUCH_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/hal/h2_pal_touch.h"

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lvgl_touch h2_lvgl_touch_t;

typedef struct h2_lvgl_touch_config {
    /** Borrowed Touch provider that must outlive the returned adapter. */
    const h2_pal_touch_api_t *touch;
    /** Borrowed allocator that must outlive the returned adapter. */
    const h2_pal_mem_api_t *allocator;
    /** Borrowed display that must outlive the returned adapter. */
    lv_display_t *display;
} h2_lvgl_touch_config_t;

/**
 * Create one LVGL pointer indev backed by a borrowed Touch PAL.
 *
 * The Touch viewport must exactly match display resolution.
 */
h2_pal_result_t h2_lvgl_touch_create(
    const h2_lvgl_touch_config_t *config,
    h2_lvgl_touch_t **out_touch);

/** Return the latest provider result observed by the LVGL read callback. */
h2_pal_result_t h2_lvgl_touch_last_result(const h2_lvgl_touch_t *touch);

/** Delete the LVGL indev, close Touch PAL, and release adapter state. */
void h2_lvgl_touch_destroy(h2_lvgl_touch_t *touch);

#ifdef __cplusplus
}
#endif

#endif
