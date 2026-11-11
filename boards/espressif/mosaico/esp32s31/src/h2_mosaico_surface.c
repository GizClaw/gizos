#include "h2_mosaico_surface.h"
#include <string.h>

int h2_mosaico_surface_blit(uint16_t* surface, const h2_display_rect_t* rect,
                            const void* pixels, size_t stride,
                            h2_display_rect_t* dirty) {
  if (!surface || !rect || !pixels || !dirty || rect->x < 0 || rect->y < 0 ||
      rect->width <= 0 || rect->height <= 0 || rect->width > H2_MOSAICO_WIDTH ||
      rect->height > H2_MOSAICO_HEIGHT ||
      rect->x > H2_MOSAICO_WIDTH - rect->width ||
      rect->y > H2_MOSAICO_HEIGHT - rect->height)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t bytes = (size_t)rect->width * sizeof(uint16_t);
  if (stride < bytes ||
      (rect->height > 1 &&
       stride > (SIZE_MAX - bytes) / (size_t)(rect->height - 1)))
    return H2_PAL_ERR_INVALID_ARG;
  for (int y = 0; y < rect->height; ++y) {
    const uint8_t* source = (const uint8_t*)pixels + (size_t)y * stride;
    uint16_t* dest =
        surface + (size_t)(rect->y + y) * H2_MOSAICO_WIDTH + rect->x;
    for (int x = 0; x < rect->width; ++x) {
      uint16_t pixel;
      memcpy(&pixel, source + (size_t)x * sizeof(pixel), sizeof(pixel));
      dest[x] = (uint16_t)((pixel << 8) | (pixel >> 8));
    }
  }
  const int first_x =
      rect->x / H2_MOSAICO_COLUMN_ALIGNMENT * H2_MOSAICO_COLUMN_ALIGNMENT;
  const int end_x = (rect->x + rect->width + H2_MOSAICO_COLUMN_ALIGNMENT - 1) /
                    H2_MOSAICO_COLUMN_ALIGNMENT * H2_MOSAICO_COLUMN_ALIGNMENT;
  const int first_y = rect->y / H2_MOSAICO_DMA_ROWS * H2_MOSAICO_DMA_ROWS;
  const int end_y = (rect->y + rect->height + H2_MOSAICO_DMA_ROWS - 1) /
                    H2_MOSAICO_DMA_ROWS * H2_MOSAICO_DMA_ROWS;
  *dirty =
      (h2_display_rect_t){first_x, first_y, end_x - first_x, end_y - first_y};
  return H2_PAL_OK;
}
