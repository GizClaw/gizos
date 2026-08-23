#ifndef H2_LVGL_DISPLAY_INTERNAL_H
#define H2_LVGL_DISPLAY_INTERNAL_H

#include "h2_lvgl_display.h"

struct h2_lvgl_display {
  h2_pal_display_t *display;
  const h2_pal_mem_api_t *allocator;
  lv_display_t *lvgl;
  void *draw_buffer;
  h2_pal_result_t last_result;
  int opened;
};

#endif
