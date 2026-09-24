#include "h2_raster2d.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void test_palette(void) {
  /* Independent nearest-integer reference over every channel pair/progress. */
  for (unsigned bits = 5; bits <= 6; ++bits) {
    unsigned max = (1u << bits) - 1u;
    for (unsigned a = 0; a <= max; ++a) {
      for (unsigned b = 0; b <= max; ++b) {
        uint16_t av =
            bits == 6 ? (uint16_t)(a << 5) : (uint16_t)((a << 11) | a);
        uint16_t bv =
            bits == 6 ? (uint16_t)(b << 5) : (uint16_t)((b << 11) | b);
        for (unsigned t = 0; t <= 256; ++t) {
          uint16_t out = 0;
          unsigned expected =
              (unsigned)((a * (256.0 - t) + b * (double)t) / 256.0 + 0.5);
          uint16_t pixel = bits == 6 ? (uint16_t)(expected << 5)
                                     : (uint16_t)((expected << 11) | expected);
          assert(h2_raster2d_palette_blend(&av, &bv, 1, t, &out, 1) ==
                 H2_PAL_OK);
          assert(out == pixel);
        }
      }
    }
  }
  uint16_t a[] = {0xf800, 0x001f, 0xaaaa}, b[] = {0x07e0, 0xffff, 0x5555};
  uint16_t reference[3];
  assert(h2_raster2d_palette_blend(a, b, 3, 128, reference, 3) == H2_PAL_OK);
  assert(h2_raster2d_palette_blend(a, b, 3, 128, a, 3) == H2_PAL_OK);
  assert(memcmp(a, reference, sizeof(a)) == 0);
  assert(h2_raster2d_palette_blend(a, a, 3, 200, a, 3) == H2_PAL_OK);
  assert(memcmp(a, reference, sizeof(a)) == 0);
  assert(h2_raster2d_palette_blend(a, b, 2, 128, a + 1, 2) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_raster2d_palette_blend(a, b, 3, 257, a, 3) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_raster2d_palette_blend(a, b, 3, 128, a, 2) == H2_PAL_ERR_NO_SPACE);
  assert(memcmp(a, reference, sizeof(a)) == 0);
  assert(h2_raster2d_palette_blend(NULL, NULL, 0, 0, NULL, 0) == H2_PAL_OK);
  assert(h2_raster2d_palette_blend(NULL, NULL, 0, 257, NULL, 0) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_raster2d_palette_blend(NULL, b, 1, 0, a, 3) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_raster2d_palette_blend(a, b, H2_RASTER2D_PALETTE_LIMIT + 1u, 0, a,
                                   SIZE_MAX) == H2_PAL_ERR_NO_SPACE);
}

static uint32_t seed = 17u;
static uint32_t next_value(void) {
  seed = seed * 1664525u + 1013904223u;
  return seed;
}

static void test_replay(void) {
  enum { WIDTH = 19, HEIGHT = 13, STRIDE = 23, COUNT = 80 };
  uint16_t actual[STRIDE * HEIGHT + 2], expected[STRIDE * HEIGHT + 2];
  uint16_t palette[] = {0xf800, 0x07e0, 0x001f};
  h2_raster2d_rect_t rects[COUNT];
  h2_raster2d_surface_t surface = {actual + 1, STRIDE * HEIGHT, WIDTH, HEIGHT,
                                   STRIDE};
  for (int trial = 0; trial < 100; ++trial) {
    for (size_t p = 0; p < sizeof(actual) / sizeof(actual[0]); ++p)
      actual[p] = expected[p] = 0x1234;
    h2_raster2d_clip_t clip = {3, 2, 17, 11};
    for (size_t i = 0; i < COUNT; ++i) {
      rects[i] = (h2_raster2d_rect_t){(int32_t)(next_value() % 60u) - 30,
                                      (int32_t)(next_value() % 40u) - 20,
                                      next_value() % 24u, next_value() % 20u,
                                      next_value() % 3u};
      /* Brute-force per-pixel oracle does not share clipping/fill code. */
      for (size_t y = clip.top; y < clip.bottom; ++y) {
        for (size_t x = clip.left; x < clip.right; ++x) {
          if ((int64_t)x >= rects[i].x &&
              (int64_t)x < (int64_t)rects[i].x + rects[i].width &&
              (int64_t)y >= rects[i].y &&
              (int64_t)y < (int64_t)rects[i].y + rects[i].height)
            expected[1 + y * STRIDE + x] = palette[rects[i].palette_index];
        }
      }
    }
    assert(h2_raster2d_draw_rects(&surface, rects, COUNT, palette, 3, &clip) ==
           H2_PAL_OK);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
    rects[COUNT - 1].palette_index = 3;
    assert(h2_raster2d_draw_rects(&surface, rects, COUNT, palette, 3, &clip) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
  }
  h2_raster2d_rect_t huge = {INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX, 0};
  assert(h2_raster2d_draw_rects(&surface, &huge, 1, palette, 3, NULL) ==
         H2_PAL_OK);
  for (size_t y = 0; y < HEIGHT; ++y)
    for (size_t x = 0; x < WIDTH; ++x)
      assert(actual[1 + y * STRIDE + x] == palette[0]);
  assert(actual[0] == 0x1234 && actual[STRIDE * HEIGHT + 1] == 0x1234);
  memcpy(expected, actual, sizeof(actual));
  assert(h2_raster2d_draw_rects(&surface, &huge, 1, actual + 1, 3, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  surface.capacity_pixels = 1;
  assert(h2_raster2d_draw_rects(&surface, &huge, 1, palette, 3, NULL) ==
         H2_PAL_ERR_NO_SPACE);
  surface.capacity_pixels = STRIDE * HEIGHT;
  surface.stride_pixels = SIZE_MAX;
  assert(h2_raster2d_draw_rects(&surface, &huge, 1, palette, 3, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  surface.stride_pixels = STRIDE;
  h2_raster2d_clip_t bad_clip = {5, 0, 4, HEIGHT};
  assert(h2_raster2d_draw_rects(&surface, &huge, 1, palette, 3, &bad_clip) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(memcmp(actual, expected, sizeof(actual)) == 0);
  assert(h2_raster2d_draw_rects(&surface, NULL, 0, NULL, 0, NULL) == H2_PAL_OK);
  h2_raster2d_surface_t empty = {NULL, 0, 0, 0, 0};
  assert(h2_raster2d_draw_rects(&empty, NULL, 0, NULL, 0, NULL) == H2_PAL_OK);
  huge.palette_index = 3;
  assert(h2_raster2d_draw_rects(&empty, &huge, 1, palette, 3, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_palette();
  test_replay();
  puts("raster2d pixels, palette and bounds: PASS");
  return 0;
}
