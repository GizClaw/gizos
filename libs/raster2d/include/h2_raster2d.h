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

/** Immutable straight (not premultiplied) RGBA8 bytes, row-major. Dimensions
 * are 1..4096, stride/capacity in bytes. No allocation or ownership transfer.
 * Pixel centers are (x+.5,y+.5); byte order is R,G,B,A on every host. */
typedef struct h2_raster2d_texture {
  const uint8_t *rgba;
  size_t capacity_bytes, width, height, stride_bytes;
} h2_raster2d_texture_t;

/** Atlas source rectangle, zero-based and half-open, wholly inside texture.
 * Source texel (x,y) occupies local [x-anchor_x,x+1-anchor_x) relative to
 * the atlas rectangle, not the full atlas. Matrix maps local edges to screen:
 * X=a*x+c*y+tx, Y=b*x+d*y+ty. All numbers finite, magnitude <= 1e6.
 * Full affine including shear and reflection. Exactly singular matrices draw
 * nothing (determinant uses double arithmetic; underflow to zero counts as
 * singular). Nonzero determinants whose inverse is nonfinite are rejected.
 * Nearest sampling inverse-maps destination centers and floors source coords;
 * atlas neighbors never bleed. Identity/integer translations align exactly.
 */
typedef struct h2_raster2d_sprite {
  h2_raster2d_texture_t texture;
  size_t x, y, width, height;
  double anchor_x, anchor_y;
  double matrix[6];
} h2_raster2d_sprite_t;
#define H2_RASTER2D_SPRITE_LIMIT 4096u

/** Validate the entire ordered batch, even clipped/singular items. Same errors,
 * borrowing, overlap, serialization and failure atomicity as draw_rects.
 * Surface/input descriptors and RGBA data must not overlap output storage.
 * Transparent samples preserve destination. Other samples quantize RGB8 to
 * R5/G6/B5 by truncation, then straight source-over each native channel using
 * (src*alpha+dst*(255-alpha)+127)/255. This is not RGBA-byte equivalence or
 * gamma-correct compositing. No allocation, retained state or display submit.
 */
h2_pal_result_t h2_raster2d_draw_sprites(const h2_raster2d_surface_t *surface,
                                         const h2_raster2d_sprite_t *sprites,
                                         size_t count,
                                         const h2_raster2d_clip_t *clip);

/** Resource/matrix validation without drawing; useful for transactional
 * producer publication. NULL is invalid. Does not inspect pixel values. */
h2_pal_result_t h2_raster2d_sprite_validate(const h2_raster2d_sprite_t *sprite);

#ifdef __cplusplus
}
#endif
#endif
