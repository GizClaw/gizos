#include "h2_lvgl_display_internal.h"

#include <limits.h>
#include <string.h>

static void display_flush(lv_display_t *lvgl, const lv_area_t *area,
                          uint8_t *pixels) {
  h2_lvgl_display_t *adapter = lv_display_get_driver_data(lvgl);
  if (adapter == NULL || area == NULL || pixels == NULL) {
    lv_display_flush_ready(lvgl);
    return;
  }
  const h2_display_rect_t rect = {
      .x = area->x1,
      .y = area->y1,
      .width = area->x2 - area->x1 + 1,
      .height = area->y2 - area->y1 + 1,
  };
  const uint32_t stride = lv_draw_buf_width_to_stride(
      (uint32_t)rect.width, LV_COLOR_FORMAT_RGB565);
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
      adapter->display, &rect, pixels, stride, H2_DISPLAY_PIXEL_RGB565);
  if (result == H2_PAL_OK && lv_display_flush_is_last(lvgl)) {
    result = (h2_pal_result_t)h2_pal_display_present(adapter->display);
  }
  if (adapter->last_result == H2_PAL_OK && result != H2_PAL_OK) {
    adapter->last_result = result;
  }
  lv_display_flush_ready(lvgl);
}

h2_pal_result_t h2_lvgl_display_create(
    const h2_lvgl_display_config_t *config,
    h2_lvgl_display_t **out_display) {
  if (out_display == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_display = NULL;
  if (config == NULL || config->display == NULL ||
      config->allocator == NULL || !lv_is_initialized()) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int result = h2_pal_display_open(config->display);
  if (result != H2_DISPLAY_OK) {
    return (h2_pal_result_t)result;
  }
  h2_display_info_t info = {0};
  result = h2_pal_display_get_info(config->display, &info);
  if (result != H2_DISPLAY_OK || info.width <= 0 || info.height <= 0) {
    (void)h2_pal_display_close(config->display);
    return result == H2_DISPLAY_OK ? H2_PAL_ERR_INVALID_STATE
                                   : (h2_pal_result_t)result;
  }
  h2_lvgl_display_t *adapter =
      h2_pal_mem_alloc(config->allocator, sizeof(*adapter));
  if (adapter == NULL) {
    (void)h2_pal_display_close(config->display);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(adapter, 0, sizeof(*adapter));
  adapter->display = config->display;
  adapter->allocator = config->allocator;
  adapter->opened = 1;
  adapter->last_result = H2_PAL_OK;
  uint32_t rows = config->draw_buffer_rows == 0u
                      ? 32u
                      : config->draw_buffer_rows;
  if (rows > (uint32_t)info.height) {
    rows = (uint32_t)info.height;
  }
  const uint32_t stride = lv_draw_buf_width_to_stride(
      (uint32_t)info.width, LV_COLOR_FORMAT_RGB565);
  if (rows == 0u || stride == 0u || stride > SIZE_MAX / rows) {
    h2_lvgl_display_destroy(adapter);
    return H2_PAL_ERR_INVALID_STATE;
  }
  const size_t draw_size = (size_t)stride * rows;
  if (draw_size > UINT32_MAX) {
    h2_lvgl_display_destroy(adapter);
    return H2_PAL_ERR_INVALID_STATE;
  }
  adapter->draw_buffer = h2_pal_mem_alloc(config->allocator, draw_size);
  if (adapter->draw_buffer == NULL) {
    h2_lvgl_display_destroy(adapter);
    return H2_PAL_ERR_NO_MEMORY;
  }
  adapter->lvgl = lv_display_create(info.width, info.height);
  if (adapter->lvgl == NULL) {
    h2_lvgl_display_destroy(adapter);
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_display_set_driver_data(adapter->lvgl, adapter);
  lv_display_set_color_format(adapter->lvgl, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(adapter->lvgl, display_flush);
  lv_display_set_buffers(adapter->lvgl, adapter->draw_buffer, NULL,
                         (uint32_t)draw_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
  *out_display = adapter;
  return H2_PAL_OK;
}

lv_display_t *h2_lvgl_display_lvgl(h2_lvgl_display_t *display) {
  return display == NULL ? NULL : display->lvgl;
}

h2_pal_result_t h2_lvgl_display_last_result(
    const h2_lvgl_display_t *display) {
  return display == NULL ? H2_PAL_ERR_INVALID_ARG : display->last_result;
}

void h2_lvgl_display_destroy(h2_lvgl_display_t *display) {
  if (display == NULL) {
    return;
  }
  if (display->lvgl != NULL && lv_is_initialized()) {
    lv_display_delete(display->lvgl);
    display->lvgl = NULL;
  }
  if (display->opened) {
    (void)h2_pal_display_close(display->display);
  }
  const h2_pal_mem_api_t *allocator = display->allocator;
  h2_pal_mem_free(allocator, display->draw_buffer);
  memset(display, 0, sizeof(*display));
  h2_pal_mem_free(allocator, display);
}
