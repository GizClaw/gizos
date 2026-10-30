#include "h2_esp_board_display_buffer.h"
#include "h2_esp_board_config.h"

#include <stdint.h>
#include <string.h>

static uint16_t to_rgb565(const uint8_t* pixel,
                          h2_display_pixel_format_t format) {
  uint16_t value;
  if (format == H2_DISPLAY_PIXEL_RGB888) {
    return (uint16_t)(((uint16_t)(pixel[0] & 0xf8u) << 8) |
                      ((uint16_t)(pixel[1] & 0xfcu) << 3) | (pixel[2] >> 3));
  }
  memcpy(&value, pixel, sizeof(value));
  if (format == H2_DISPLAY_PIXEL_RGB444) {
    const uint16_t r = (value >> 8) & 0x0fu;
    const uint16_t g = (value >> 4) & 0x0fu;
    const uint16_t b = value & 0x0fu;
    value = (uint16_t)((((r << 1) | (r >> 3)) << 11) |
                       (((g << 2) | (g >> 2)) << 5) | (b << 1) | (b >> 3));
  }
  return value;
}

int h2_ws175c_display_buffer_update(uint16_t* shadow,
                                    const h2_display_rect_t* rect,
                                    const void* pixels, size_t stride_bytes,
                                    h2_display_pixel_format_t format,
                                    h2_display_rect_t* transfer) {
  if (shadow == NULL || rect == NULL || pixels == NULL || transfer == NULL ||
      rect->width <= 0 || rect->height <= 0) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  size_t pixel_bytes;
  if (format == H2_DISPLAY_PIXEL_RGB888) {
    pixel_bytes = 3u;
  } else if (format == H2_DISPLAY_PIXEL_RGB565 ||
             format == H2_DISPLAY_PIXEL_RGB444) {
    pixel_bytes = 2u;
  } else {
    return H2_DISPLAY_ERR_UNSUPPORTED;
  }
  if ((size_t)rect->width > SIZE_MAX / pixel_bytes) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  const size_t row_bytes = (size_t)rect->width * pixel_bytes;
  if (stride_bytes < row_bytes ||
      (size_t)(rect->height - 1) > (SIZE_MAX - row_bytes) / stride_bytes) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  const int64_t right = (int64_t)rect->x + rect->width;
  const int64_t bottom = (int64_t)rect->y + rect->height;
  const int32_t x1 = rect->x < 0 ? 0 : rect->x;
  const int32_t y1 = rect->y < 0 ? 0 : rect->y;
  const int32_t x2 =
      right > H2_WS175C_LCD_WIDTH ? H2_WS175C_LCD_WIDTH : (int32_t)right;
  const int32_t y2 =
      bottom > H2_WS175C_LCD_HEIGHT ? H2_WS175C_LCD_HEIGHT : (int32_t)bottom;
  if (x1 >= x2 || y1 >= y2) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }

  for (int32_t y = y1; y < y2; ++y) {
    const uint8_t* row = (const uint8_t*)pixels +
                         (size_t)((int64_t)y - rect->y) * stride_bytes +
                         (size_t)((int64_t)x1 - rect->x) * pixel_bytes;
    for (int32_t x = x1; x < x2; ++x) {
      const uint16_t value = to_rgb565(row, format);
      shadow[(size_t)y * H2_WS175C_LCD_WIDTH + (size_t)x] =
          (uint16_t)((value << 8) | (value >> 8));
      row += pixel_bytes;
    }
  }
  *transfer = (h2_display_rect_t){
      .x = x1 & ~1,
      .y = y1 & ~1,
      .width = ((x2 + 1) & ~1) - (x1 & ~1),
      .height = ((y2 + 1) & ~1) - (y1 & ~1),
  };
  return H2_DISPLAY_OK;
}
