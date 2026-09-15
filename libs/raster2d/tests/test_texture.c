#include "h2_raster2d.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static uint16_t blend(uint16_t bg, const uint8_t *p) {
  unsigned c = 0;
  const unsigned shift[] = {11, 5, 0}, bits[] = {5, 6, 5};
  for (int k = 0; k < 3; k++) {
    unsigned max = (1u << bits[k]) - 1;
    unsigned dst = (bg >> shift[k]) & max, src = p[k] >> (8 - bits[k]);
    c |= ((src * p[3] + dst * (255 - p[3]) + 127) / 255) << shift[k];
  }
  return (uint16_t)c;
}
/* Slow forward-cell oracle: test the destination center against each texel's
 * transformed half-open parallelogram, rather than inverse stepping a bbox. */
static void oracle(uint16_t *out, const h2_raster2d_sprite_t *s,
                   h2_raster2d_clip_t clip) {
  const double *m = s->matrix;
  double det = m[0] * m[3] - m[1] * m[2];
  if (det == 0)
    return;
  for (size_t y = clip.top; y < clip.bottom; y++)
    for (size_t x = clip.left; x < clip.right; x++)
      for (size_t v = 0; v < s->height; v++)
        for (size_t u = 0; u < s->width; u++) {
          double px = (double)u - s->anchor_x, py = (double)v - s->anchor_y;
          double dx = x + .5 - (m[0] * px + m[2] * py + m[4]);
          double dy = y + .5 - (m[1] * px + m[3] * py + m[5]);
          double U = (dx * m[3] - dy * m[2]) / det,
                 V = (dy * m[0] - dx * m[1]) / det;
          if (U >= 0 && U < 1 && V >= 0 && V < 1)
            out[y * 13 + x] =
                blend(out[y * 13 + x],
                      s->texture.rgba + (s->y + v) * s->texture.stride_bytes +
                          (s->x + u) * 4);
        }
}
int main(void) {
  uint8_t rgba[4 * 5 * 4];
  for (size_t i = 0; i < 20; i++) {
    rgba[i * 4] = (uint8_t)(i * 11);
    rgba[i * 4 + 1] = (uint8_t)(255 - i * 7);
    rgba[i * 4 + 2] = (uint8_t)(i * 13);
    rgba[i * 4 + 3] = (uint8_t[]){0, 128, 255, 1, 254}[i % 5];
  }
  h2_raster2d_sprite_t s = {.texture = {rgba, sizeof(rgba), 5, 4, 20},
                            .x = 1,
                            .y = 1,
                            .width = 3,
                            .height = 2,
                            .matrix = {1, 0, 0, 1, 0, 0}};
  const double matrices[][6] = {
      {1, 0, 0, 1, 0, 0},   {1, 0, 0, 1, 3, 4},
      {0, 1, -1, 0, 6, 2},  {2, 0, 0, 3, 1, 1},
      {-2, 0, 0, 2, 9, 1},  {1, 0, .5, 1, 3, 2},
      {0, 0, 0, 1, 4, 4},   {1, 2, 2, 4, 0, 0},
      {.5, 0, 0, .5, 4, 4}, {.8, .6, -.6, .8, 5.125, 3.25},
      {1, 0, 0, 1, -2, -1}};
  size_t cases = 0;
  for (size_t i = 0; i < sizeof(matrices) / sizeof(matrices[0]); i++)
    for (int clipped = 0; clipped < 2; clipped++)
      for (int anchored = 0; anchored < 2; anchored++) {
        uint16_t got[13 * 12 + 2], expected[13 * 12 + 2];
        for (size_t j = 0; j < sizeof(got) / sizeof(*got); j++)
          got[j] = expected[j] = 0x39e7;
        memcpy(s.matrix, matrices[i], sizeof(s.matrix));
        s.anchor_x = anchored ? 1 : 0;
        s.anchor_y = anchored ? .5 : 0;
        h2_raster2d_surface_t surface = {got + 1, 13 * 12, 12, 12, 13};
        h2_raster2d_clip_t clip = clipped ? (h2_raster2d_clip_t){2, 3, 8, 9}
                                          : (h2_raster2d_clip_t){0, 0, 12, 12};
        oracle(expected + 1, &s, clip);
        assert(h2_raster2d_draw_sprites(&surface, &s, 1, &clip) == H2_PAL_OK);
        if (memcmp(got, expected, sizeof(got))) {
          fprintf(stderr, "case %zu/%d/%d\n", i, clipped, anchored);
          assert(0);
        }
        cases++;
      }
  {
    uint16_t pixels[4] = {0x1234, 0x1234, 0x1234, 0x1234};
    h2_raster2d_surface_t dst = {pixels, 4, 2, 2, 2};
    h2_raster2d_sprite_t transparent = s;
    transparent.x = transparent.y = 0;
    transparent.width = transparent.height = 1;
    transparent.anchor_x = transparent.anchor_y = 0;
    memcpy(transparent.matrix, matrices[0], sizeof(transparent.matrix));
    assert(h2_raster2d_draw_sprites(&dst, &transparent, 1, NULL) == H2_PAL_OK);
    for (size_t i = 0; i < 4; i++)
      assert(pixels[i] == 0x1234);
  }
  uint16_t pixels[16], before[16];
  memset(pixels, 0x55, sizeof(pixels));
  memcpy(before, pixels, sizeof(pixels));
  h2_raster2d_surface_t dst = {pixels, 16, 4, 4, 4};
  s.anchor_x = s.anchor_y = 0;
  memcpy(s.matrix, matrices[0], sizeof(s.matrix));
  h2_raster2d_sprite_t batch[2] = {s, s};
  for (int k = 0; k < 10; k++) {
    batch[1] = s;
    switch (k) {
    case 0:
      batch[1].x = 5;
      break;
    case 1:
      batch[1].texture.capacity_bytes = 1;
      break;
    case 2:
      batch[1].matrix[0] = NAN;
      break;
    case 3:
      batch[1].matrix[4] = INFINITY;
      break;
    case 4:
      batch[1].texture.stride_bytes = SIZE_MAX;
      break;
    case 5:
      batch[1].texture.rgba = NULL;
      break;
    case 6:
      batch[1].width = SIZE_MAX;
      break;
    case 7:
      batch[1].anchor_y = NAN;
      break;
    case 8:
      batch[1].matrix[0] = 1e7;
      break;
    case 9:
      batch[1].texture.rgba = (const uint8_t *)pixels;
      break;
    }
    assert(h2_raster2d_draw_sprites(&dst, batch, 2, NULL) != H2_PAL_OK);
    assert(!memcmp(pixels, before, sizeof(pixels)));
  }
  assert(h2_raster2d_draw_sprites(&dst, NULL, 0, NULL) == H2_PAL_OK);
  assert(h2_raster2d_draw_sprites(&dst, &s, H2_RASTER2D_SPRITE_LIMIT + 1,
                                  NULL) == H2_PAL_ERR_NO_SPACE);
  h2_raster2d_clip_t bad = {0, 0, 5, 4};
  assert(h2_raster2d_draw_sprites(&dst, &s, 1, &bad) == H2_PAL_ERR_INVALID_ARG);
  assert(!memcmp(pixels, before, sizeof(pixels)));
  /* Extremely small but invertible scales are accepted when inverse entries
   * remain finite; reciprocal(det) alone can overflow and must not reject. */
  s.matrix[0] = s.matrix[3] = 1e-160;
  assert(h2_raster2d_sprite_validate(&s) == H2_PAL_OK);
  assert(h2_raster2d_draw_sprites(&dst, &s, 1, NULL) == H2_PAL_OK);
  assert(!memcmp(pixels, before, sizeof(pixels)));
  s.matrix[0] = 1e-320;
  s.matrix[3] = 1;
  assert(h2_raster2d_sprite_validate(&s) == H2_PAL_ERR_INVALID_ARG);
  s.matrix[0] = s.matrix[3] = 0;
  s.x = 5;
  assert(h2_raster2d_draw_sprites(&dst, &s, 1, NULL) != H2_PAL_OK);
  assert(!memcmp(pixels, before, sizeof(pixels)));
  printf("Texture forward-cell oracle PASS cases=%zu; guards and atomic errors "
         "PASS\n",
         cases);
  return 0;
}
