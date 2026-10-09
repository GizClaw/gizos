#include "h2_mosaico_surface.h"
#include <string.h>

int h2_mosaico_surface_blit(uint16_t *surface, const h2_display_rect_t *rect,
                           const void *pixels, size_t stride, int *first, int *end) {
    if (!surface || !rect || !pixels || !first || !end || rect->x < 0 || rect->y < 0 ||
        rect->width <= 0 || rect->height <= 0 || rect->width > H2_MOSAICO_WIDTH ||
        rect->height > H2_MOSAICO_HEIGHT || rect->x > H2_MOSAICO_WIDTH - rect->width ||
        rect->y > H2_MOSAICO_HEIGHT - rect->height) return H2_PAL_ERR_INVALID_ARG;
    const size_t bytes = (size_t)rect->width * sizeof(uint16_t);
    if (stride < bytes || (rect->height > 1 &&
        stride > (SIZE_MAX - bytes) / (size_t)(rect->height - 1))) return H2_PAL_ERR_INVALID_ARG;
    for (int y = 0; y < rect->height; ++y) {
        const uint8_t *source = (const uint8_t *)pixels + (size_t)y * stride;
        uint16_t *dest = surface + (size_t)(rect->y + y) * H2_MOSAICO_WIDTH + rect->x;
        for (int x = 0; x < rect->width; ++x) {
            uint16_t pixel;
            memcpy(&pixel, source + (size_t)x * sizeof(pixel), sizeof(pixel));
            dest[x] = (uint16_t)((pixel << 8) | (pixel >> 8));
        }
    }
    *first = rect->y / H2_MOSAICO_DMA_ROWS * H2_MOSAICO_DMA_ROWS;
    *end = (rect->y + rect->height + H2_MOSAICO_DMA_ROWS - 1) /
           H2_MOSAICO_DMA_ROWS * H2_MOSAICO_DMA_ROWS;
    return H2_PAL_OK;
}
