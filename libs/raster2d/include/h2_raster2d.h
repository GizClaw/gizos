#ifndef H2_RASTER2D_H
#define H2_RASTER2D_H

/** @file h2_raster2d.h
 * @brief Allocation-free RGB565 rectangle replay and palette interpolation.
 */
#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_RASTER2D_RECT_LIMIT 16384u
#define H2_RASTER2D_PALETTE_LIMIT 16384u

/** Caller-owned native RGB565 pixels; capacity and stride are in pixels.
 * Width/height must fit INT32_MAX and stride must be at least width. Nonempty
 * surfaces require aligned storage for (height - 1) * stride + width pixels.
 * The descriptor and input arrays must not overlap framebuffer storage.
 */
typedef struct h2_raster2d_surface {
  uint16_t *pixels;
  size_t capacity_pixels;
  size_t width;
  size_t height;
  size_t stride_pixels;
} h2_raster2d_surface_t;

/** Half-open rectangle; coordinates may be outside the surface. */
typedef struct h2_raster2d_rect {
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
  uint32_t palette_index; /**< Zero-based index into the supplied palette. */
} h2_raster2d_rect_t;

/** Half-open clip bounds, entirely within the surface; empty is valid. */
typedef struct h2_raster2d_clip {
  size_t left;
  size_t top;
  size_t right;
  size_t bottom;
} h2_raster2d_clip_t;

/**
 * @brief Blend R5/G6/B5 channels into caller-provided storage.
 * @param a First palette, containing count aligned native RGB565 entries.
 * @param b Second palette, containing count aligned native RGB565 entries.
 * @param count Number of entries, at most H2_RASTER2D_PALETTE_LIMIT.
 * @param progress Integer 0..256, including exact A/B endpoints.
 * @param out Output, allowed to exactly alias either input; partial overlap
 * fails.
 * @param out_capacity Available output entries, not bytes.
 * @return OK, INVALID_ARG for invalid progress/pointers/overlap/address
 * arithmetic, or NO_SPACE for insufficient output capacity or excessive count.
 * Errors leave output unchanged. NULL arrays are permitted only with count
 * zero.
 *
 * Each channel is (a * (256 - progress) + b * progress + 128) >> 8.
 * Synchronous, no allocation or retained pointers, not ISR-safe. The caller
 * serializes mutations; all borrowed storage remains valid throughout the call.
 */
h2_pal_result_t h2_raster2d_palette_blend(const uint16_t *a, const uint16_t *b,
                                          size_t count, unsigned progress,
                                          uint16_t *out, size_t out_capacity);

/**
 * @brief Replay rectangles in order; later rectangles overwrite earlier ones.
 * @param surface Required caller-owned surface descriptor, borrowed for the
 * call.
 * @param rects Aligned input array with rect_count entries; NULL only if empty.
 * @param rect_count Number of rectangles, at most H2_RASTER2D_RECT_LIMIT.
 * @param palette Aligned RGB565 input, NULL only with zero palette_count.
 * @param palette_count Palette length, at most H2_RASTER2D_PALETTE_LIMIT.
 * @param clip Optional half-open clip; NULL selects the full surface.
 * @return OK, INVALID_ARG for malformed inputs, indices, clip, overlap or
 * address overflow, or NO_SPACE for insufficient surface capacity/excessive
 * counts. All inputs are validated before any write, including clipped-away
 * rectangles. Errors leave pixels unchanged. Empty surfaces/batches/clips and
 * zero-area rectangles write nothing. Arbitrary invalid pointers cannot be
 * detected.
 *
 * Synchronous, allocation-free, no retained state, display submission, damage
 * tracking or callbacks. Caller serializes writes and keeps descriptors and
 * inputs unchanged throughout the call; no input may overlap framebuffer
 * storage. Native RGB565 values are not a serialized byte stream. Not ISR-safe.
 */
h2_pal_result_t h2_raster2d_draw_rects(const h2_raster2d_surface_t *surface,
                                       const h2_raster2d_rect_t *rects,
                                       size_t rect_count,
                                       const uint16_t *palette,
                                       size_t palette_count,
                                       const h2_raster2d_clip_t *clip);

#ifdef __cplusplus
}
#endif
#endif
