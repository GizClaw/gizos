#ifndef H2_MOSAICO_SURFACE_H
#define H2_MOSAICO_SURFACE_H
#include "h2/pal/hal/h2_pal_display.h"
enum { H2_MOSAICO_WIDTH = 480, H2_MOSAICO_HEIGHT = 480, H2_MOSAICO_DMA_ROWS = 8 };
/* Merge host-endian RGB565 into a wire-endian shadow and return aligned rows.
 * Caller owns WIDTH * HEIGHT pixels; invalid requests leave it unchanged. */
int h2_mosaico_surface_blit(uint16_t *surface, const h2_display_rect_t *rect,
                           const void *pixels, size_t stride, int *first, int *end);
#endif
