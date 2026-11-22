#include "h2_esp_board_display_config_internal.h"

#include <limits.h>
#include <string.h>

static int landscape(h2_esp_board_display_orientation_t orientation) {
  return orientation == H2_ESP_BOARD_DISPLAY_LANDSCAPE_CCW;
}

void h2_esp_board_display_extent(h2_esp_board_display_orientation_t orientation,
                                 int32_t* width, int32_t* height) {
  *width =
      landscape(orientation) ? H2_AMOLED_NATIVE_HEIGHT : H2_AMOLED_NATIVE_WIDTH;
  *height =
      landscape(orientation) ? H2_AMOLED_NATIVE_WIDTH : H2_AMOLED_NATIVE_HEIGHT;
}

void h2_esp_board_display_touch_point(
    h2_esp_board_display_orientation_t orientation, int32_t* x, int32_t* y) {
  if (landscape(orientation)) {
    const int32_t native_x = *x;
    *x = *y;
    *y = H2_AMOLED_NATIVE_WIDTH - 1 - native_x;
  }
}

static inline uint16_t panel_pixel(const uint8_t* pixel,
                                   h2_display_pixel_format_t format) {
  uint16_t rgb565;
  if (format == H2_DISPLAY_PIXEL_RGB888) {
    rgb565 = (uint16_t)((((uint16_t)pixel[0] & 0xf8u) << 8) |
                        (((uint16_t)pixel[1] & 0xfcu) << 3) | (pixel[2] >> 3));
  } else {
    memcpy(&rgb565, pixel, sizeof(rgb565));
    if (format == H2_DISPLAY_PIXEL_RGB444) {
      const uint16_t r = (rgb565 >> 8) & 0x0fu;
      const uint16_t g = (rgb565 >> 4) & 0x0fu;
      const uint16_t b = rgb565 & 0x0fu;
      rgb565 = (uint16_t)((((r << 1) | (r >> 3)) << 11) |
                          (((g << 2) | (g >> 2)) << 5) | (b << 1) | (b >> 3));
    }
  }
  return (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
}

h2_display_rect_t h2_esp_board_display_pack(
    h2_esp_board_display_orientation_t orientation,
    const h2_display_rect_t* source, const h2_display_rect_t* chunk,
    const void* pixels, size_t stride_bytes, h2_display_pixel_format_t format,
    size_t pixel_size, uint16_t* out) {
  const uint8_t* start = (const uint8_t*)pixels;
  start += (size_t)((int64_t)chunk->y - source->y) * stride_bytes;
  start += (size_t)((int64_t)chunk->x - source->x) * pixel_size;

  if (landscape(orientation)) {
    /* The panel keeps its native GRAM order. Each native row walks up one
     * logical column, the inverse of the touch transform. */
    for (int32_t x = 0; x < chunk->width; ++x) {
      const uint8_t* column = start + (size_t)x * pixel_size;
      for (int32_t y = chunk->height; y > 0; --y) {
        *out++ = panel_pixel(column + (size_t)(y - 1) * stride_bytes, format);
      }
    }
    return (h2_display_rect_t){
        H2_AMOLED_NATIVE_WIDTH - chunk->y - chunk->height, chunk->x,
        chunk->height, chunk->width};
  }

  for (int32_t y = 0; y < chunk->height; ++y) {
    const uint8_t* row = start + (size_t)y * stride_bytes;
    for (int32_t x = 0; x < chunk->width; ++x) {
      *out++ = panel_pixel(row + (size_t)x * pixel_size, format);
    }
  }
  return *chunk;
}

int h2_esp_board_display_config_is_valid(
    const h2_esp_board_display_config_t* config) {
  if (config == NULL || config->pclk_hz > (uint32_t)INT_MAX ||
      (config->orientation != H2_ESP_BOARD_DISPLAY_PORTRAIT &&
       !landscape(config->orientation))) {
    return 0;
  }
  uint32_t rows = config->dma_buffer_rows;
  if (rows != 0u && (rows < 8u || rows > 64u || (rows & (rows - 1u)) != 0u)) {
    return 0;
  }
  return 1;
}

int h2_esp_board_display_config_may_apply(int already_initialized) {
  return already_initialized == 0;
}

uint32_t h2_esp_board_display_dma_rows(
    const h2_esp_board_display_config_t* config, uint32_t default_rows) {
  if (!h2_esp_board_display_config_is_valid(config) || default_rows < 8u ||
      default_rows > 64u || (default_rows & (default_rows - 1u)) != 0u) {
    return 0u;
  }
  return config->dma_buffer_rows != 0u ? config->dma_buffer_rows : default_rows;
}
