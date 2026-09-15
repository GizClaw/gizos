#include "h2_raster2d.h"
#include "h2_raster2d_internal.h"

#include <limits.h>
#include <math.h>

/* Integer ranges avoid relational comparison of unrelated C pointers. They
 * validate representability/overlap, not whether an arbitrary address is live.
 */
static int valid_range(const void *p, size_t count, size_t element_size,
                       size_t alignment) {
  if (count == 0u)
    return 1;
  return p != NULL && (uintptr_t)p % alignment == 0u &&
         count <= SIZE_MAX / element_size &&
         count * element_size <= UINTPTR_MAX - (uintptr_t)p;
}

static int overlaps(const void *a, size_t a_bytes, const void *b,
                    size_t b_bytes) {
  if (a_bytes == 0u || b_bytes == 0u)
    return 0;
  return (uintptr_t)a < (uintptr_t)b + b_bytes &&
         (uintptr_t)b < (uintptr_t)a + a_bytes;
}

h2_pal_result_t h2_raster2d_palette_blend(const uint16_t *a, const uint16_t *b,
                                          size_t count, unsigned progress,
                                          uint16_t *out, size_t out_capacity) {
  if (progress > 256u)
    return H2_PAL_ERR_INVALID_ARG;
  if (count > H2_RASTER2D_PALETTE_LIMIT || count > out_capacity)
    return H2_PAL_ERR_NO_SPACE;
  if (!valid_range(a, count, sizeof(*a), _Alignof(uint16_t)) ||
      !valid_range(b, count, sizeof(*b), _Alignof(uint16_t)) ||
      !valid_range(out, count, sizeof(*out), _Alignof(uint16_t)))
    return H2_PAL_ERR_INVALID_ARG;
  size_t bytes = count * sizeof(*out);
  if ((out != a && overlaps(out, bytes, a, bytes)) ||
      (out != b && overlaps(out, bytes, b, bytes)))
    return H2_PAL_ERR_INVALID_ARG;
  unsigned inverse = 256u - progress;
  for (size_t i = 0u; i < count; ++i) {
    unsigned av = a[i], bv = b[i];
    unsigned r = ((av >> 11) * inverse + (bv >> 11) * progress + 128u) >> 8;
    unsigned g =
        (((av >> 5) & 63u) * inverse + ((bv >> 5) & 63u) * progress + 128u) >>
        8;
    unsigned bl = ((av & 31u) * inverse + (bv & 31u) * progress + 128u) >> 8;
    out[i] = (uint16_t)((r << 11) | (g << 5) | bl);
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_raster2d_draw_rects(const h2_raster2d_surface_t *surface,
                                       const h2_raster2d_rect_t *rects,
                                       size_t rect_count,
                                       const uint16_t *palette,
                                       size_t palette_count,
                                       const h2_raster2d_clip_t *clip) {
  if (surface == NULL || surface->width > INT32_MAX ||
      surface->height > INT32_MAX || surface->stride_pixels < surface->width)
    return H2_PAL_ERR_INVALID_ARG;
  if (rect_count > H2_RASTER2D_RECT_LIMIT ||
      palette_count > H2_RASTER2D_PALETTE_LIMIT)
    return H2_PAL_ERR_NO_SPACE;
  if (!valid_range(rects, rect_count, sizeof(*rects),
                   _Alignof(h2_raster2d_rect_t)) ||
      !valid_range(palette, palette_count, sizeof(*palette),
                   _Alignof(uint16_t)))
    return H2_PAL_ERR_INVALID_ARG;
  h2_raster2d_clip_t bounds = {0u, 0u, surface->width, surface->height};
  if (clip != NULL)
    bounds = *clip;
  if (bounds.left > bounds.right || bounds.right > surface->width ||
      bounds.top > bounds.bottom || bounds.bottom > surface->height)
    return H2_PAL_ERR_INVALID_ARG;
  int nonempty = surface->width != 0u && surface->height != 0u;
  if (nonempty) {
    if ((surface->height - 1u) >
        (SIZE_MAX - surface->width) / surface->stride_pixels)
      return H2_PAL_ERR_INVALID_ARG;
    size_t needed =
        (surface->height - 1u) * surface->stride_pixels + surface->width;
    if (needed > surface->capacity_pixels)
      return H2_PAL_ERR_NO_SPACE;
    if (!valid_range(surface->pixels, surface->capacity_pixels,
                     sizeof(uint16_t), _Alignof(uint16_t)))
      return H2_PAL_ERR_INVALID_ARG;
    size_t bytes = surface->capacity_pixels * sizeof(uint16_t);
    if (overlaps(surface->pixels, bytes, rects, rect_count * sizeof(*rects)) ||
        overlaps(surface->pixels, bytes, palette,
                 palette_count * sizeof(*palette)) ||
        overlaps(surface->pixels, bytes, surface, sizeof(*surface)) ||
        (clip != NULL && overlaps(surface->pixels, bytes, clip, sizeof(*clip))))
      return H2_PAL_ERR_INVALID_ARG;
  }
  for (size_t i = 0u; i < rect_count; ++i) {
    if (rects[i].palette_index >= palette_count)
      return H2_PAL_ERR_INVALID_ARG;
  }
  if (!nonempty || bounds.left == bounds.right || bounds.top == bounds.bottom)
    return H2_PAL_OK;
  for (size_t i = 0u; i < rect_count; ++i) {
    const h2_raster2d_rect_t *rect = &rects[i];
    int64_t left = rect->x, top = rect->y;
    int64_t right = left + rect->width, bottom = top + rect->height;
    if (left < (int64_t)bounds.left)
      left = (int64_t)bounds.left;
    if (top < (int64_t)bounds.top)
      top = (int64_t)bounds.top;
    if (right > (int64_t)bounds.right)
      right = (int64_t)bounds.right;
    if (bottom > (int64_t)bounds.bottom)
      bottom = (int64_t)bounds.bottom;
    if (left >= right || top >= bottom)
      continue;
    uint16_t color = palette[rect->palette_index];
    for (size_t y = (size_t)top; y < (size_t)bottom; ++y) {
      uint16_t *pixels =
          surface->pixels + y * surface->stride_pixels + (size_t)left;
      h2_raster2d_fill_span_unchecked(pixels, (size_t)(right - left), color);
    }
  }
  return H2_PAL_OK;
}

static int sprite_number(double x) {
  return isfinite(x) && fabs(x) <= 1000000.;
}

h2_pal_result_t h2_raster2d_sprite_validate(const h2_raster2d_sprite_t *s) {
  if (!s)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_raster2d_texture_t *t = &s->texture;
  if (!t->width || !t->height || t->width > 4096 || t->height > 4096 ||
      t->stride_bytes < t->width * 4 ||
      t->height - 1 > (SIZE_MAX - t->width * 4) / t->stride_bytes)
    return H2_PAL_ERR_INVALID_ARG;
  if ((t->height - 1) * t->stride_bytes + t->width * 4 > t->capacity_bytes)
    return H2_PAL_ERR_NO_SPACE;
  if (!valid_range(t->rgba, t->capacity_bytes, 1, 1) || s->x > t->width ||
      s->width > t->width - s->x || s->y > t->height ||
      s->height > t->height - s->y || !sprite_number(s->anchor_x) ||
      !sprite_number(s->anchor_y))
    return H2_PAL_ERR_INVALID_ARG;
  for (int i = 0; i < 6; i++)
    if (!sprite_number(s->matrix[i]))
      return H2_PAL_ERR_INVALID_ARG;
  const double *m = s->matrix;
  double det = m[0] * m[3] - m[1] * m[2];
  if (det != 0 && (!isfinite(m[0] / det) || !isfinite(m[1] / det) ||
                   !isfinite(m[2] / det) || !isfinite(m[3] / det)))
    return H2_PAL_ERR_INVALID_ARG;
  return H2_PAL_OK;
}

h2_pal_result_t h2_raster2d_draw_sprites(const h2_raster2d_surface_t *surface,
                                         const h2_raster2d_sprite_t *sprites,
                                         size_t count,
                                         const h2_raster2d_clip_t *clip) {
  if (count > H2_RASTER2D_SPRITE_LIMIT)
    return H2_PAL_ERR_NO_SPACE;
  if (!valid_range(sprites, count, sizeof(*sprites),
                   _Alignof(h2_raster2d_sprite_t)))
    return H2_PAL_ERR_INVALID_ARG;
  /* Reuse the established surface, clip and descriptor validation. Empty
   * rectangle replay cannot write or require a palette. */
  h2_pal_result_t rc = h2_raster2d_draw_rects(surface, NULL, 0, NULL, 0, clip);
  if (rc)
    return rc;
  size_t bytes =
      (surface->width && surface->height) ? surface->capacity_pixels * 2 : 0;
  if (overlaps(surface->pixels, bytes, sprites, count * sizeof(*sprites)))
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < count; i++) {
    rc = h2_raster2d_sprite_validate(&sprites[i]);
    if (rc)
      return rc;
    if (overlaps(surface->pixels, bytes, sprites[i].texture.rgba,
                 sprites[i].texture.capacity_bytes))
      return H2_PAL_ERR_INVALID_ARG;
  }
  h2_raster2d_clip_t c = {0, 0, surface->width, surface->height};
  if (clip)
    c = *clip;
  for (size_t i = 0; i < count; i++) {
    const h2_raster2d_sprite_t *s = &sprites[i];
    const double *m = s->matrix;
    double det = m[0] * m[3] - m[1] * m[2];
    if (det == 0 || !s->width || !s->height)
      continue;
    double l = INFINITY, t = INFINITY, r = -INFINITY, b = -INFINITY;
    for (int k = 0; k < 4; k++) {
      double x = (k & 1 ? (double)s->width : 0) - s->anchor_x;
      double y = (k & 2 ? (double)s->height : 0) - s->anchor_y;
      double X = m[0] * x + m[2] * y + m[4], Y = m[1] * x + m[3] * y + m[5];
      l = fmin(l, X);
      r = fmax(r, X);
      t = fmin(t, Y);
      b = fmax(b, Y);
    }
    /* Clamp in double before integer conversion, including enormous bounds. */
    l = fmax((double)c.left, floor(l));
    r = fmin((double)c.right, ceil(r));
    t = fmax((double)c.top, floor(t));
    b = fmin((double)c.bottom, ceil(b));
    if (l >= r || t >= b)
      continue;
    double ia = m[3] / det, ib = -m[1] / det, ic = -m[2] / det, id = m[0] / det;
    for (size_t y = (size_t)t; y < (size_t)b; y++) {
      for (size_t x = (size_t)l; x < (size_t)r; x++) {
        double dx = (double)x + .5 - m[4], dy = (double)y + .5 - m[5];
        double u = ia * dx + ic * dy + s->anchor_x,
               v = ib * dx + id * dy + s->anchor_y;
        /* Comparisons reject NaNs from extreme inverse intermediate math. */
        if (!(u >= 0 && u < (double)s->width && v >= 0 &&
              v < (double)s->height))
          continue;
        const uint8_t *p = s->texture.rgba +
                           (s->y + (size_t)v) * s->texture.stride_bytes +
                           (s->x + (size_t)u) * 4;
        unsigned a = p[3];
        if (!a)
          continue;
        uint16_t *out = surface->pixels + y * surface->stride_pixels + x;
        unsigned old = *out, inv = 255 - a;
        unsigned R = ((p[0] >> 3) * a + (old >> 11) * inv + 127) / 255;
        unsigned G = ((p[1] >> 2) * a + ((old >> 5) & 63) * inv + 127) / 255;
        unsigned B = ((p[2] >> 3) * a + (old & 31) * inv + 127) / 255;
        *out = (uint16_t)((R << 11) | (G << 5) | B);
      }
    }
  }
  return H2_PAL_OK;
}
