#include "h2_qrcode_example.h"

#include <stdint.h>
#include <string.h>

/* Bands keep the render buffer small enough for internal RAM on every board. */
#define H2_QRCODE_EXAMPLE_BAND_ROWS 16
#define H2_QRCODE_EXAMPLE_DEFAULT_MAX_VERSION 10
#define H2_QRCODE_EXAMPLE_DARK_COLOR 0x0000u
#define H2_QRCODE_EXAMPLE_LIGHT_COLOR 0xFFFFu

static h2_pal_result_t present_qrcode(h2_runtime_t *runtime,
                                      const h2_qrcode_t *qrcode,
                                      const h2_display_info_t *info,
                                      int quiet_modules) {
  const size_t band_pixels =
      (size_t)info->width * (size_t)H2_QRCODE_EXAMPLE_BAND_ROWS;
  h2_qrcode_layout_t layout;
  uint16_t *pixels = NULL;
  h2_pal_result_t result = H2_PAL_OK;

  result = h2_qrcode_layout_center(qrcode, info->width, info->height,
                                   quiet_modules, &layout);
  if (result != H2_PAL_OK) {
    return result;
  }
  pixels = (uint16_t *)h2_pal_mem_alloc(runtime->mem,
                                        band_pixels * sizeof(uint16_t));
  if (pixels == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }

  for (int y = 0; y < info->height; y += H2_QRCODE_EXAMPLE_BAND_ROWS) {
    int rows = info->height - y;
    h2_display_rect_t rect;

    if (rows > H2_QRCODE_EXAMPLE_BAND_ROWS) {
      rows = H2_QRCODE_EXAMPLE_BAND_ROWS;
    }
    result = h2_qrcode_render_rgb565_band(
        qrcode, &layout, H2_QRCODE_EXAMPLE_DARK_COLOR,
        H2_QRCODE_EXAMPLE_LIGHT_COLOR, H2_QRCODE_EXAMPLE_LIGHT_COLOR, y,
        info->width, rows, pixels, band_pixels);
    if (result != H2_PAL_OK) {
      break;
    }
    rect.x = 0;
    rect.y = y;
    rect.width = info->width;
    rect.height = rows;
    result = h2_pal_display_draw_bitmap(runtime->display, &rect, pixels,
                                        (size_t)info->width * sizeof(uint16_t),
                                        H2_DISPLAY_PIXEL_RGB565);
    if (result != H2_DISPLAY_OK) {
      break;
    }
  }
  h2_pal_mem_free(runtime->mem, pixels);
  if (result != H2_PAL_OK) {
    return result;
  }
  return h2_pal_display_present(runtime->display);
}

h2_pal_result_t
h2_qrcode_example_run(h2_runtime_t *runtime,
                      const h2_qrcode_example_config_t *config) {
  int max_version = H2_QRCODE_EXAMPLE_DEFAULT_MAX_VERSION;
  int quiet_modules = H2_QRCODE_QUIET_MODULES_MIN;
  size_t buffer_len = 0u;
  uint8_t *modules = NULL;
  uint8_t *scratch = NULL;
  h2_qrcode_t qrcode;
  h2_display_info_t info;
  h2_pal_result_t result = H2_PAL_OK;

  if (runtime == NULL || config == NULL || config->text == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (runtime->display == NULL || runtime->mem == NULL) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (config->max_version != 0) {
    max_version = config->max_version;
  }
  if (config->quiet_modules != 0) {
    quiet_modules = config->quiet_modules;
  }
  if (max_version < H2_QRCODE_VERSION_MIN ||
      max_version > H2_QRCODE_VERSION_MAX ||
      quiet_modules < H2_QRCODE_QUIET_MODULES_MIN) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  buffer_len = (size_t)H2_QRCODE_BUFFER_LEN_FOR_VERSION(max_version);
  modules = (uint8_t *)h2_pal_mem_alloc(runtime->mem, buffer_len);
  scratch = (uint8_t *)h2_pal_mem_alloc(runtime->mem, buffer_len);
  if (modules == NULL || scratch == NULL) {
    h2_pal_mem_free(runtime->mem, modules);
    h2_pal_mem_free(runtime->mem, scratch);
    return H2_PAL_ERR_NO_MEMORY;
  }
  result = h2_qrcode_encode_text(config->text, config->ecc, max_version,
                                 modules, buffer_len, scratch, buffer_len,
                                 &qrcode);
  h2_pal_mem_free(runtime->mem, scratch);
  if (result != H2_PAL_OK) {
    h2_pal_mem_free(runtime->mem, modules);
    return result;
  }

  result = h2_pal_display_open(runtime->display);
  if (result != H2_DISPLAY_OK) {
    h2_pal_mem_free(runtime->mem, modules);
    return result;
  }
  memset(&info, 0, sizeof(info));
  result = h2_pal_display_get_info(runtime->display, &info);
  if (result != H2_DISPLAY_OK) {
    h2_pal_mem_free(runtime->mem, modules);
    return result;
  }
  if (info.width <= 0 || info.height <= 0) {
    h2_pal_mem_free(runtime->mem, modules);
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  if (config->brightness_percent != 0u) {
    (void)h2_pal_display_set_brightness_percent(runtime->display,
                                                config->brightness_percent);
  }

  result = present_qrcode(runtime, &qrcode, &info, quiet_modules);
  h2_pal_mem_free(runtime->mem, modules);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (config->on_ready != NULL) {
    return config->on_ready(config->on_ready_user);
  }
  return H2_PAL_OK;
}
