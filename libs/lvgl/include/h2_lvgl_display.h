#ifndef H2_LVGL_DISPLAY_H
#define H2_LVGL_DISPLAY_H

#include "h2/pal/hal/h2_pal_display.h"
#include "h2/pal/os/h2_pal_mem.h"

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lvgl_display h2_lvgl_display_t;

typedef struct h2_lvgl_display_config {
  /** Borrowed Display provider that must outlive the adapter. */
  h2_pal_display_t *display;
  /** Borrowed allocator that must outlive the adapter. */
  const h2_pal_mem_api_t *allocator;
  /** Partial-render buffer row count; zero selects 32 rows. */
  uint32_t draw_buffer_rows;
} h2_lvgl_display_config_t;

h2_pal_result_t h2_lvgl_display_create(
    const h2_lvgl_display_config_t *config,
    h2_lvgl_display_t **out_display);
lv_display_t *h2_lvgl_display_lvgl(h2_lvgl_display_t *display);
h2_pal_result_t h2_lvgl_display_last_result(
    const h2_lvgl_display_t *display);
void h2_lvgl_display_destroy(h2_lvgl_display_t *display);

#ifdef __cplusplus
}
#endif

#endif
