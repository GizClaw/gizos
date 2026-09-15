#ifndef H2_RASTER2D_INTERNAL_H
#define H2_RASTER2D_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Implementation-only sharing with the Lua Display adapter. The caller has
 * already validated storage and clipped the span. No checks or state here. */
static inline void h2_raster2d_fill_span_unchecked(uint16_t *pixels,
                                                   size_t count,
                                                   uint16_t color) {
  while (count >= 4u) {
    pixels[0] = color;
    pixels[1] = color;
    pixels[2] = color;
    pixels[3] = color;
    pixels += 4;
    count -= 4u;
  }
  while (count != 0u) {
    *pixels++ = color;
    --count;
  }
}

#endif
