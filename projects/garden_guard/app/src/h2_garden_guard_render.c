#include "h2_garden_guard_render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BAND_ROWS H2_GARDEN_GUARD_BAND_ROWS
#define RGB H2_GARDEN_GUARD_RGB
#define LANES H2_GARDEN_GUARD_LANES
#define COLS H2_GARDEN_GUARD_COLS
#define COOLDOWN_STEPS 20u
#define PROGRESS_STEPS 40u

/* Courtyard geometry beyond the lawn in ground units: the flower bed behind
 * the far edge, the curb stones around the lawn and the house wall left of the
 * patio. Heights of the wall, its roof, the back fence and the hedge are in
 * near lawn widths. */
#define BED_DEPTH 0.16f
#define CURB 0.012f
#define WALL_U (-0.09f)
#define WALL_HEIGHT 0.3f
#define ROOF_HEIGHT 0.1f
#define FENCE_HEIGHT 0.07f
#define HEDGE_HEIGHT 0.045f

enum { DEPTH_OFF = 0, DEPTH_WRITE, DEPTH_TEST };

typedef struct region {
  int32_t x0, y0, x1, y1; /* Half-open, clipped to the canvas. */
} region_t;

/* 5x7 glyphs, one byte per row, bit 4 is the leftmost column. */
static const uint8_t k_digits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

static const uint8_t k_letters[26][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};

static const uint8_t k_plus[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
static const uint8_t k_question[7] = {0x0e, 0x11, 0x02, 0x04, 0x04, 0x00, 0x04};
static const uint8_t k_bang[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
static const uint8_t k_slash[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
static const uint8_t k_comma[7] = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08};

static const uint8_t* glyph(char c) {
  if (c >= '0' && c <= '9') return k_digits[c - '0'];
  if (c >= 'A' && c <= 'Z') return k_letters[c - 'A'];
  if (c >= 'a' && c <= 'z') return k_letters[c - 'a'];
  switch (c) {
    case '+':
      return k_plus;
    case '?':
      return k_question;
    case '!':
      return k_bang;
    case '/':
      return k_slash;
    case ',':
      return k_comma;
    default:
      return NULL;
  }
}

/* Both languages use the same RGB565 composition and geometry. */
#define L(en, zh) (game->language == H2_GARDEN_GUARD_LANG_ZH ? (zh) : (en))

static uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

static float clampf(float value, float low, float high) {
  if (!(value >= low)) return low;
  return value > high ? high : value;
}

static float fractf(float value) { return value - floorf(value); }

/* Pixel distance to the nearest integer line of a coordinate, given how many
 * pixels one unit of it spans here. */
static float seam_px(float value, float px_per_unit) {
  const float d = fractf(value);
  return fminf(d, 1.0f - d) * px_per_unit;
}

static uint16_t blend(uint16_t dst, uint16_t src, uint32_t alpha) {
  if (alpha >= 256u) return src;
  if (alpha == 0u) return dst;
  const uint32_t inverse = 256u - alpha;
  const uint32_t r =
      ((dst >> 11) * inverse + (uint32_t)(src >> 11) * alpha) >> 8;
  const uint32_t g =
      (((dst >> 5) & 63u) * inverse + (uint32_t)((src >> 5) & 63u) * alpha) >>
      8;
  const uint32_t b =
      ((dst & 31u) * inverse + (uint32_t)(src & 31u) * alpha) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Mix toward white by amount/256, used for hit flashes and highlights. */
static uint16_t lighten(uint16_t color, uint32_t amount) {
  return blend(color, 0xFFFFu, amount);
}

/* Mix toward black by amount/256. */
static uint16_t darken(uint16_t color, uint32_t amount) {
  return blend(color, 0x0000u, amount);
}

/* Multiply every channel by k, saturating. */
static uint16_t scale565(uint16_t color, float k) {
  int32_t r = (int32_t)((float)(color >> 11) * k + 0.5f);
  int32_t g = (int32_t)((float)((color >> 5) & 63u) * k + 0.5f);
  int32_t b = (int32_t)((float)(color & 31u) * k + 0.5f);
  r = r < 0 ? 0 : (r > 31 ? 31 : r);
  g = g < 0 ? 0 : (g > 63 ? 63 : g);
  b = b < 0 ? 0 : (b > 31 ? 31 : b);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

uint64_t h2_garden_guard_all_bands(int32_t height) {
  if (height <= 0) return 0;
  const int32_t bands = (height + BAND_ROWS - 1) / BAND_ROWS;
  if (bands >= H2_GARDEN_GUARD_MAX_BANDS) return UINT64_MAX;
  return ((uint64_t)1 << bands) - 1u;
}

/* Clip a float box, record its bands and report whether to paint it. */
static bool region_begin(h2_garden_guard_canvas_t* canvas, float fx0, float fy0,
                         float fx1, float fy1, region_t* out) {
  const float w = (float)canvas->width;
  const float top = (float)canvas->clip_y0;
  const float bottom =
      canvas->clip_y1 > 0 ? (float)canvas->clip_y1 : (float)canvas->height;
  const int32_t x0 = (int32_t)floorf(clampf(fx0, 0.0f, w));
  const int32_t y0 = (int32_t)floorf(clampf(fy0, top, bottom));
  const int32_t x1 = (int32_t)ceilf(clampf(fx1, 0.0f, w));
  const int32_t y1 = (int32_t)ceilf(clampf(fy1, top, bottom));
  if (x0 >= x1 || y0 >= y1) return false;
  for (int32_t band = y0 / BAND_ROWS;
       band <= (y1 - 1) / BAND_ROWS && band < H2_GARDEN_GUARD_MAX_BANDS;
       ++band) {
    const uint64_t bit = (uint64_t)1 << band;
    if (!(canvas->bands & bit)) {
      canvas->bands |= bit;
      canvas->band_x0[band] = (int16_t)x0;
      canvas->band_x1[band] = (int16_t)x1;
    } else {
      if (x0 < canvas->band_x0[band]) canvas->band_x0[band] = (int16_t)x0;
      if (x1 > canvas->band_x1[band]) canvas->band_x1[band] = (int16_t)x1;
    }
  }
  *out = (region_t){x0, y0, x1, y1};
  return !canvas->measure && canvas->pixels != NULL;
}

static uint16_t* row_at(const h2_garden_guard_canvas_t* canvas, int32_t y) {
  return canvas->pixels + (size_t)y * canvas->stride_pixels;
}

/* Occlusion row for the current mode, or NULL when occlusion is off. */
static uint8_t* depth_row(const h2_garden_guard_canvas_t* canvas, int32_t y) {
  if (canvas->depth == NULL || canvas->depth_mode == DEPTH_OFF) return NULL;
  return canvas->depth + (size_t)y * canvas->stride_pixels;
}

/* Blend one pixel, honouring the occlusion mode: the scene skips pixels a
 * nearer lane owns, and mostly opaque static pixels claim their lane. */
static void plot(const h2_garden_guard_canvas_t* canvas, uint16_t* row,
                 uint8_t* depth, int32_t x, uint16_t color, uint32_t alpha) {
  if (depth != NULL) {
    if (canvas->depth_mode == DEPTH_TEST) {
      if (depth[x] > canvas->depth_layer) return;
    } else if (alpha >= 128u) {
      depth[x] = canvas->depth_layer;
    }
  }
  row[x] = blend(row[x], color, alpha);
}

static void fill_rect(h2_garden_guard_canvas_t* canvas, float x0, float y0,
                      float x1, float y1, uint16_t color, uint32_t alpha) {
  if (alpha == 0u) return;
  region_t g;
  if (!region_begin(canvas, x0, y0, x1, y1, &g)) return;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    for (int32_t px = g.x0; px < g.x1; ++px)
      plot(canvas, row, depth, px, color, alpha);
  }
}

static void disc(h2_garden_guard_canvas_t* canvas, float cx, float cy, float r,
                 uint16_t color, uint32_t alpha) {
  if (!(r > 0.0f) || alpha == 0u) return;
  region_t g;
  if (!region_begin(canvas, cx - r - 1.0f, cy - r - 1.0f, cx + r + 1.0f,
                    cy + r + 1.0f, &g))
    return;
  const float outer = (r + 0.5f) * (r + 0.5f);
  const float inner = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : 0.0f;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    const float dy = (float)py + 0.5f - cy;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float dx = (float)px + 0.5f - cx;
      const float d2 = dx * dx + dy * dy;
      if (d2 >= outer) continue;
      const float cover = d2 <= inner ? 1.0f : r + 0.5f - sqrtf(d2);
      plot(canvas, row, depth, px, color, (uint32_t)(cover * (float)alpha));
    }
  }
}

/* Axis-aligned ellipse with an approximate one-pixel antialiased edge. */
static void ellipse(h2_garden_guard_canvas_t* canvas, float cx, float cy,
                    float rx, float ry, uint16_t color, uint32_t alpha) {
  if (!(rx > 0.0f) || !(ry > 0.0f) || alpha == 0u) return;
  region_t g;
  if (!region_begin(canvas, cx - rx - 1.0f, cy - ry - 1.0f, cx + rx + 1.0f,
                    cy + ry + 1.0f, &g))
    return;
  const float edge = fminf(rx, ry);
  const float irx = 1.0f / rx, iry = 1.0f / ry;
  const float half = 0.5f / edge;
  const float inside = fmaxf(0.0f, 1.0f - half - 0.0001f);
  const float outside = 1.0f + half + 0.0001f;
  const float inner2 = inside * inside, outer2 = outside * outside;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    const float ny = ((float)py + 0.5f - cy) * iry;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float nx = ((float)px + 0.5f - cx) * irx;
      const float d2 = nx * nx + ny * ny;
      /* Only the one-pixel transition needs a square root. Keep a margin
       * at the endpoints so the same float coverage rounds to the same ink. */
      if (d2 >= outer2) continue;
      const float cover =
          d2 < inner2 ? 1.0f
                      : clampf((1.0f - sqrtf(d2)) * edge + 0.5f, 0.0f, 1.0f);
      if (cover > 0.0f)
        plot(canvas, row, depth, px, color, (uint32_t)(cover * (float)alpha));
    }
  }
}

static void capsule(h2_garden_guard_canvas_t* canvas, float x0, float y0,
                    float x1, float y1, float r, uint16_t color,
                    uint32_t alpha) {
  if (!(r > 0.0f) || alpha == 0u) return;
  region_t g;
  if (!region_begin(canvas, fminf(x0, x1) - r - 1.0f, fminf(y0, y1) - r - 1.0f,
                    fmaxf(x0, x1) + r + 1.0f, fmaxf(y0, y1) + r + 1.0f, &g))
    return;
  const float sx = x1 - x0, sy = y1 - y0;
  const float length2 = sx * sx + sy * sy;
  const float inverse_length2 = length2 > 0.0f ? 1.0f / length2 : 0.0f;
  const float outer = (r + 0.5f) * (r + 0.5f);
  const float inner = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : 0.0f;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    const float py0 = (float)py + 0.5f - y0;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float px0 = (float)px + 0.5f - x0;
      const float t =
          clampf((px0 * sx + py0 * sy) * inverse_length2, 0.0f, 1.0f);
      const float ex = px0 - t * sx, ey = py0 - t * sy;
      const float d2 = ex * ex + ey * ey;
      if (d2 >= outer) continue;
      const float cover =
          d2 <= inner ? 1.0f : clampf(r + 0.5f - sqrtf(d2), 0.0f, 1.0f);
      plot(canvas, row, depth, px, color, (uint32_t)(cover * (float)alpha));
    }
  }
}

static void rounded_rect(h2_garden_guard_canvas_t* canvas, float x0, float y0,
                         float x1, float y1, float radius, uint16_t color,
                         uint32_t alpha) {
  if (alpha == 0u) return;
  region_t g;
  if (!region_begin(canvas, x0, y0, x1, y1, &g)) return;
  const float left = x0 + radius, right = x1 - radius;
  /* Leave the endpoints to the regular coverage calculation. */
  const int32_t middle0 =
      (int32_t)ceilf(clampf(left + 0.5f, (float)g.x0, (float)g.x1));
  const int32_t middle1 =
      (int32_t)floorf(clampf(right - 0.5f, (float)middle0, (float)g.x1));
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    const float fy = (float)py + 0.5f;
    const float dy = fmaxf(fmaxf(y0 + radius - fy, fy - (y1 - radius)), 0.0f);
    for (int32_t px = g.x0; px < g.x1; ++px) {
      if (px == middle0 && middle0 < middle1) {
        if (dy <= radius)
          for (; px < middle1; ++px) plot(canvas, row, depth, px, color, alpha);
        px = middle1 - 1;
        continue;
      }
      const float fx = (float)px + 0.5f;
      const float dx = fmaxf(fmaxf(left - fx, fx - right), 0.0f);
      float cover = 1.0f;
      if (dx > 0.0f && dy > 0.0f)
        cover = clampf(radius + 0.5f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
      else if (dx > radius || dy > radius)
        cover = 0.0f;
      if (cover > 0.0f)
        plot(canvas, row, depth, px, color, (uint32_t)(cover * (float)alpha));
    }
  }
}

/* Trapezoid with horizontal top and bottom edges, such as a lawn cell. */
static void trapezoid(h2_garden_guard_canvas_t* canvas, float top, float top_x0,
                      float top_x1, float bottom, float bottom_x0,
                      float bottom_x1, uint16_t color, uint32_t alpha) {
  if (!(bottom > top) || alpha == 0u) return;
  region_t g;
  if (!region_begin(canvas, fminf(top_x0, bottom_x0) - 1.0f, top,
                    fmaxf(top_x1, bottom_x1) + 1.0f, bottom, &g))
    return;
  const float inverse_h = 1.0f / (bottom - top);
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    const float fy = (float)py + 0.5f;
    const float s = clampf((fy - top) * inverse_h, 0.0f, 1.0f);
    const float x0 = top_x0 + (bottom_x0 - top_x0) * s;
    const float x1 = top_x1 + (bottom_x1 - top_x1) * s;
    const float vertical =
        clampf(fminf(fy - top + 0.5f, bottom - fy + 0.5f), 0.0f, 1.0f);
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float fx = (float)px + 0.5f;
      const float cover =
          clampf(fminf(fx - x0 + 0.5f, x1 - fx + 0.5f), 0.0f, 1.0f) * vertical;
      if (cover > 0.0f)
        plot(canvas, row, depth, px, color, (uint32_t)(cover * (float)alpha));
    }
  }
}

/* Outlined shapes: a darker rim of width line under the fill. */
static void odisc(h2_garden_guard_canvas_t* canvas, float cx, float cy, float r,
                  float line, uint16_t fill, uint16_t rim) {
  disc(canvas, cx, cy, r + line, rim, 256u);
  disc(canvas, cx, cy, r, fill, 256u);
}

static void oellipse(h2_garden_guard_canvas_t* canvas, float cx, float cy,
                     float rx, float ry, float line, uint16_t fill,
                     uint16_t rim) {
  ellipse(canvas, cx, cy, rx + line, ry + line, rim, 256u);
  ellipse(canvas, cx, cy, rx, ry, fill, 256u);
}

static void ocapsule(h2_garden_guard_canvas_t* canvas, float x0, float y0,
                     float x1, float y1, float r, float line, uint16_t fill,
                     uint16_t rim) {
  capsule(canvas, x0, y0, x1, y1, r + line, rim, 256u);
  capsule(canvas, x0, y0, x1, y1, r, fill, 256u);
}

#include "h2_garden_guard_zh.inc"

static uint32_t next_glyph(const char** text) {
  const uint8_t* p = (const uint8_t*)*text;
  const uint8_t lead = *p++;
  if (lead < 0x80u) {
    *text = (const char*)p;
    return lead;
  }
  unsigned count = lead >= 0xc2u && lead <= 0xdfu   ? 1u
                   : lead >= 0xe0u && lead <= 0xefu ? 2u
                   : lead >= 0xf0u && lead <= 0xf4u ? 3u
                                                    : 0u;
  const unsigned bytes = count;
  uint32_t value = lead & (0x7fu >> (count + 1u));
  while (count != 0u && (*p & 0xc0u) == 0x80u) {
    value = (value << 6) | (*p++ & 0x3fu);
    --count;
  }
  *text = (const char*)p;
  const uint32_t minimum = bytes == 1u   ? 0x80u
                           : bytes == 2u ? 0x800u
                                         : 0x10000u;
  return bytes != 0u && count == 0u && value >= minimum && value <= 0x10ffffu &&
                 (value < 0xd800u || value > 0xdfffu)
             ? value
             : (uint32_t)'?';
}

static int32_t han_size(int32_t scale) {
  return scale == 1 ? HAN_SMALL_SIDE : 7 * scale;
}

static const uint32_t* han_bitmap(uint32_t code, bool small) {
  size_t lo = 0, hi = sizeof(k_han) / sizeof(k_han[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    if (k_han[mid].code < code)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo < sizeof(k_han) / sizeof(k_han[0]) && k_han[lo].code == code
             ? (small ? k_han[lo].small : k_han[lo].rows)
             : NULL;
}

int32_t h2_garden_guard_text_width(const char* text, int32_t scale) {
  if (text == NULL || *text == '\0' || scale <= 0) return 0;
  int32_t width = 0, gap = 0;
  while (*text != '\0') {
    const uint32_t code = next_glyph(&text);
    gap = code < 128u ? scale : 1;
    width += code < 128u ? 6 * scale : han_size(scale) + gap;
  }
  return width - gap;
}

static int32_t label_height(const char* label, int32_t scale) {
  for (const char* c = label; *c != '\0'; ++c)
    if ((uint8_t)*c >= 128u) return han_size(scale);
  return 7 * scale;
}

#ifdef __EMSCRIPTEN__
static void collect_menu_label(h2_garden_guard_canvas_t* canvas,
                               const char* text, float x, float y,
                               int32_t scale, uint16_t color, uint32_t alpha,
                               bool centered) {
  h2_garden_guard_menu_labels_t* menu = canvas->menu_labels;
  if (menu == NULL || menu->count >= H2_GARDEN_GUARD_MAX_MENU_LABELS) return;
  h2_garden_guard_menu_label_t* label = &menu->labels[menu->count++];
  *label = (h2_garden_guard_menu_label_t){
      .x = x,
      .y = y,
      .width = (float)h2_garden_guard_text_width(text, scale),
      .height = (float)label_height(text, scale),
      .rgb565 = color,
      .opacity = alpha,
      .centered = centered};
  (void)snprintf(label->text, sizeof(label->text), "%s", text);
}
#endif

static void text_layer(h2_garden_guard_canvas_t* canvas, const char* text,
                       int32_t x, int32_t y, int32_t scale, uint16_t color,
                       uint32_t alpha) {
#ifdef __EMSCRIPTEN__
  if (canvas->external_menu_labels) {
    collect_menu_label(canvas, text, (float)x, (float)y, scale, color, alpha,
                       false);
    return;
  }
#endif
  const int32_t line_height = label_height(text, scale);
  const int32_t font_side = scale == 1 ? HAN_SMALL_SIDE : HAN_SIDE;
  while (*text != '\0') {
    const uint32_t code = next_glyph(&text);
    const uint32_t* han = code >= 128u ? han_bitmap(code, scale == 1) : NULL;
    if (han != NULL) {
      const int32_t px = han_size(scale);
      region_t bounds;
      if (region_begin(canvas, (float)x, (float)y, (float)(x + px),
                       (float)(y + px), &bounds)) {
        for (int32_t row = bounds.y0; row < bounds.y1; ++row) {
          uint16_t* dest = row_at(canvas, row);
          uint8_t* depth = depth_row(canvas, row);
          const uint32_t bits = han[((row - y) * 2 + 1) * font_side / (px * 2)];
          for (int32_t col = bounds.x0; col < bounds.x1; ++col) {
            const int32_t sx = ((col - x) * 2 + 1) * font_side / (px * 2);
            const uint32_t coverage = (bits >> (2 * sx)) & 3u;
            if (coverage != 0u)
              plot(canvas, dest, depth, col, color, alpha * coverage / 3u);
          }
        }
      }
      x += px + 1;
      continue;
    }
    const uint8_t* rows = glyph(code < 128u ? (char)code : '?');
    if (rows != NULL) {
      const int32_t baseline = y + (line_height - 7 * scale) / 2;
      for (int32_t row = 0; row < 7; ++row)
        for (int32_t column = 0; column < 5; ++column)
          if (rows[row] & (0x10u >> column))
            fill_rect(canvas, (float)(x + column * scale),
                      (float)(baseline + row * scale),
                      (float)(x + (column + 1) * scale),
                      (float)(baseline + (row + 1) * scale), color, alpha);
    }
    x += code < 128u ? 6 * scale : han_size(scale) + 1;
  }
}

/* Text with a soft drop shadow; (x, y) is the top-left corner. */
static void text(h2_garden_guard_canvas_t* canvas, const char* value, int32_t x,
                 int32_t y, int32_t scale, uint16_t color, uint32_t alpha) {
#ifdef __EMSCRIPTEN__
  if (canvas->external_menu_labels) {
    collect_menu_label(canvas, value, (float)x, (float)y, scale, color, alpha,
                       false);
    return;
  }
#endif
  const int32_t offset = scale / 2 > 0 ? scale / 2 : 1;
  const char* ascii = value;
  while (*ascii != '\0' && (uint8_t)*ascii < 128u) ++ascii;
  if (*ascii == '\0')
    text_layer(canvas, value, x + offset, y + offset, scale, RGB(20, 16, 8),
               alpha * 3u / 4u);
  text_layer(canvas, value, x, y, scale, color, alpha);
}

static void text_centered(h2_garden_guard_canvas_t* canvas, const char* value,
                          float cx, float y, int32_t scale, uint16_t color,
                          uint32_t alpha) {
#ifdef __EMSCRIPTEN__
  if (canvas->external_menu_labels) {
    collect_menu_label(canvas, value, cx, y, scale, color, alpha, true);
    return;
  }
#endif
  const int32_t width = h2_garden_guard_text_width(value, scale);
  text(canvas, value, (int32_t)(cx - (float)width * 0.5f), (int32_t)y, scale,
       color, alpha);
}

static int32_t text_scale(float unit, float base) {
  const int32_t scale = (int32_t)(base * unit + 0.5f);
  return scale > 0 ? scale : 1;
}

/* Largest scale up to preferred for which text fits within max_width. */
static int32_t fit_scale(const char* value, float max_width,
                         int32_t preferred) {
  int32_t scale = preferred;
  while (scale > 1 &&
         (float)h2_garden_guard_text_width(value, scale) > max_width)
    --scale;
  return scale;
}

/* ---- Figures ------------------------------------------------------------
 * Every figure stands on the ground at (gx, gy) and is sized by s, the cell
 * width at its depth. Up is -y. Plants and carts face right toward the
 * incoming pests; pests face left toward the house. */

static float rim_width(float s) { return fmaxf(1.0f, s * 0.034f); }

/* Soft contact shadow on the ground, flattened by the viewing angle. */
static void ground_shadow(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                          float rx) {
  ellipse(canvas, gx + rx * 0.08f, gy, rx, rx * 0.3f, RGB(18, 40, 14), 104u);
}

/* Short closed contours in sprite-local coordinates. A scan-converted span
 * goes through the same clip and depth operations as the round primitives. */
typedef struct art_point {
  float x, y;
} art_point_t;

#define ART_MAX_POINTS 36u

static void art_shape(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                      float scale, float ca, float sa, uint16_t color,
                      const art_point_t* points, size_t count) {
  if (count < 3u || count > ART_MAX_POINTS) return;
  art_point_t path[ART_MAX_POINTS];
  float x0 = (float)canvas->width, y0 = (float)canvas->height;
  float x1 = 0.0f, y1 = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    const float x = gx + (points[i].x * ca - points[i].y * sa) * scale;
    const float y = gy + (points[i].y * ca + points[i].x * sa) * scale;
    path[i] = (art_point_t){x, y};
    x0 = fminf(x0, x);
    y0 = fminf(y0, y);
    x1 = fmaxf(x1, x);
    y1 = fmaxf(y1, y);
  }
  region_t bounds;
  if (!region_begin(canvas, x0 - 0.5f, y0, x1 + 0.5f, y1, &bounds)) return;
  for (int32_t y = bounds.y0; y < bounds.y1; ++y) {
    const float center = (float)y + 0.5f;
    float cuts[ART_MAX_POINTS];
    size_t n = 0;
    for (size_t i = 0, prev = count - 1u; i < count; prev = i++) {
      const art_point_t a = path[prev];
      const art_point_t b = path[i];
      if ((a.y > center) == (b.y > center)) continue;
      const float cross = a.x + (b.x - a.x) * ((center - a.y) / (b.y - a.y));
      size_t at = n++;
      while (at > 0u && cuts[at - 1u] > cross) {
        cuts[at] = cuts[at - 1u];
        --at;
      }
      cuts[at] = cross;
    }
    uint16_t* row = row_at(canvas, y);
    uint8_t* depth = depth_row(canvas, y);
    for (size_t i = 0; i + 1u < n; i += 2u) {
      const float left = cuts[i], right = cuts[i + 1u];
      const int32_t first = (int32_t)fmaxf((float)bounds.x0, floorf(left));
      const int32_t last = (int32_t)fminf((float)bounds.x1, ceilf(right));
      for (int32_t x = first; x < last; ++x) {
        const float cover =
            fminf(right, (float)x + 1.0f) - fmaxf(left, (float)x);
        if (cover > 0.0f)
          plot(canvas, row, depth, x, color, (uint32_t)(256.0f * cover));
      }
    }
  }
}

/* A compound literal keeps the authored contours beside their paint layers. */
#define ART(canvas, x, y, s, ca, sa, color, ...) \
  art_shape(canvas, x, y, s, ca, sa, color,      \
            (const art_point_t[]){__VA_ARGS__},  \
            sizeof((const art_point_t[]){__VA_ARGS__}) / sizeof(art_point_t))

/* Round the corners of the small closed paths whose silhouettes need a soft
 * edge. Hard seams, cracks and highlights use the straight path instead. */
static void art_round(h2_garden_guard_canvas_t* canvas, float x, float y,
                      float scale, float ca, float sa, uint16_t color,
                      const art_point_t* points, size_t count) {
  art_point_t contour[ART_MAX_POINTS];
  if (count < 3u || count > ART_MAX_POINTS / 2u) return;
  for (size_t i = 0, prev = count - 1u; i < count; prev = i++) {
    const art_point_t a = points[prev], b = points[i];
    contour[i * 2u] =
        (art_point_t){0.74f * a.x + 0.26f * b.x, 0.74f * a.y + 0.26f * b.y};
    contour[i * 2u + 1u] =
        (art_point_t){0.26f * a.x + 0.74f * b.x, 0.26f * a.y + 0.74f * b.y};
  }
  art_shape(canvas, x, y, scale, ca, sa, color, contour, count * 2u);
}

#define ART_ROUND(canvas, x, y, s, ca, sa, color, ...) \
  art_round(canvas, x, y, s, ca, sa, color,            \
            (const art_point_t[]){__VA_ARGS__},        \
            sizeof((const art_point_t[]){__VA_ARGS__}) / sizeof(art_point_t))

static void draw_leaf(h2_garden_guard_canvas_t* canvas, float x, float y,
                      float s, float angle, uint16_t color, uint16_t rim) {
  const float ca = cosf(angle), sa = sinf(angle);
  ART_ROUND(canvas, x, y, s, ca, sa, rim, {0.01f, 0.028f}, {0.10f, -0.074f},
            {0.219f, -0.135f}, {0.335f, -0.124f}, {0.427f, -0.064f},
            {0.462f, -0.048f}, {0.411f, 0.025f}, {0.326f, 0.088f},
            {0.219f, 0.113f}, {0.106f, 0.085f});
  ART_ROUND(canvas, x, y, s, ca, sa, color, {0.055f, 0.024f}, {0.136f, -0.059f},
            {0.226f, -0.108f}, {0.328f, -0.100f}, {0.426f, -0.05f},
            {0.371f, 0.032f}, {0.308f, 0.067f}, {0.219f, 0.085f},
            {0.129f, 0.068f});
  ART_ROUND(canvas, x, y, s, ca, sa, lighten(color, 80u), {0.083f, 0.036f},
            {0.179f, -0.041f}, {0.271f, -0.073f}, {0.387f, -0.052f},
            {0.272f, -0.044f}, {0.193f, -0.013f});
}

static void draw_sunflower(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                           float s, uint32_t stage,
                           h2_garden_guard_plant_pose_t pose, bool head_only) {
  s *= pose.growth;
  const float edge = fmaxf(0.56f, s * 0.017f);
  const uint16_t green = darken(RGB(100, 159, 70), stage * 42u);
  const uint16_t leaf_ink = RGB(61, 101, 65);
  const float dx = gx + (0.016f + pose.sway) * s;
  const float dy = gy - (0.725f + pose.lift) * s;
  if (!head_only) {
    ground_shadow(canvas, gx, gy, 0.34f * s);
    draw_leaf(canvas, gx + 0.025f * s, gy - 0.13f * s, 0.84f * s,
              -0.49f - 2.3f * pose.sway, RGB(123, 177, 75), leaf_ink);
    const float tip = (dx - gx) / s;
    const float top = (dy - gy) / s + 0.15f;
    ART_ROUND(canvas, gx, gy, s, 1.0f, 0.0f, leaf_ink, {-0.059f, -0.035f},
              {-0.028f, -0.19f}, {0.008f, -0.39f}, {tip - 0.047f, top},
              {tip + 0.055f, top}, {0.095f, -0.39f}, {0.097f, -0.17f},
              {0.046f, -0.022f}, {-0.007f, 0.010f});
    ART_ROUND(canvas, gx, gy, s, 1.0f, 0.0f, green, {-0.034f, -0.036f},
              {-0.004f, -0.19f}, {0.032f, -0.39f}, {tip - 0.025f, top + 0.015f},
              {tip + 0.028f, top + 0.015f}, {0.069f, -0.39f}, {0.07f, -0.174f},
              {0.026f, -0.043f});
    draw_leaf(canvas, gx, gy - 0.23f * s, s, 3.61f + 3.8f * pose.sway,
              RGB(142, 187, 76), leaf_ink);
  }
  // Long, overlapping petals follow the turned face. The nearest petals
  // sit low on its right side, with amber shadows under the outer folds.
  static const struct {
    float ca, sa, length;
  } petals[] = {
      {0.91f, 0.42f, 0.93f},   {0.57f, 0.82f, 1.04f},   {-0.04f, 1.0f, 1.02f},
      {-0.64f, 0.77f, 1.12f},  {-0.96f, 0.27f, 1.11f},  {-0.98f, -0.19f, 1.15f},
      {-0.74f, -0.67f, 1.12f}, {-0.30f, -0.95f, 1.04f}, {0.34f, -0.94f, 1.08f},
      {0.79f, -0.62f, 0.99f},  {1.0f, -0.02f, 0.91f}};
  const uint16_t crown_edge = RGB(184, 111, 43);
  const float spread = (1.0f + pose.action * 0.12f) * s;
  const float base = dx - 0.012f * s;
  const uint32_t warm = (uint32_t)(65.0f * pose.action);
  for (size_t i = 0; i < sizeof(petals) / sizeof(petals[0]); ++i) {
    const float ca = petals[i].ca, sa = petals[i].sa;
    const float extent = spread * petals[i].length;
    ART_ROUND(canvas, base, dy, extent, ca, sa, crown_edge, {0.10f, -0.096f},
              {0.259f, -0.073f}, {0.362f, -0.071f}, {0.407f, -0.016f},
              {0.378f, 0.069f}, {0.253f, 0.083f}, {0.13f, 0.084f});
  }
  for (size_t i = 0; i < sizeof(petals) / sizeof(petals[0]); ++i) {
    const float ca = petals[i].ca, sa = petals[i].sa;
    const float extent = spread * petals[i].length;
    const uint16_t amber =
        sa + ca * 0.38f < 0.04f ? RGB(246, 172, 47) : RGB(220, 123, 41);
    ART_ROUND(canvas, base, dy, extent, ca, sa, lighten(amber, warm),
              {0.125f, -0.068f}, {0.251f, -0.063f}, {0.362f, -0.051f},
              {0.388f, -0.012f}, {0.357f, 0.054f}, {0.25f, 0.067f},
              {0.14f, 0.067f});
    ART_ROUND(canvas, base, dy, extent, ca, sa,
              sa < 0.25f ? RGB(255, 211, 93) : RGB(250, 165, 55),
              {0.158f, -0.044f}, {0.274f, -0.039f}, {0.347f, -0.018f},
              {0.315f, 0.006f}, {0.230f, 0.021f});
  }
  oellipse(canvas, dx, dy + 0.004f * s, 0.219f * s, 0.214f * s, edge,
           RGB(202, 124, 63), RGB(137, 75, 45));
  ellipse(canvas, dx + 0.008f * s, dy - 0.035f * s, 0.165f * s, 0.159f * s,
          RGB(242, 173, 91), 256u);
  const uint16_t ink = RGB(100, 56, 43);
  for (int side = -1; side <= 1; side += 2) {
    const float eye_x = dx + (0.008f + (float)side * 0.079f) * s;
    const float eye_y = dy - (side < 0 ? 0.055f : 0.037f) * s;
    const float open_height = (side < 0 ? 0.060f : 0.049f) * s;
    if (pose.blink) {
      capsule(canvas, eye_x - 0.036f * s, eye_y, eye_x + 0.03f * s,
              eye_y + 0.009f * s, 0.014f * s, ink, 256u);
    } else {
      oellipse(canvas, eye_x, eye_y, 0.048f * s, open_height, 0.5f,
               RGB(255, 228, 172), ink);
      ellipse(canvas, eye_x + 0.014f * s, eye_y + 0.006f * s, 0.027f * s,
              open_height * 0.75f, RGB(75, 48, 40), 256u);
      disc(canvas, eye_x + 0.006f * s, eye_y - 0.012f * s, 0.011f * s,
           RGB(255, 250, 220), 256u);
    }
    ellipse(canvas, dx + (0.004f + (float)side * 0.129f) * s, dy + 0.050f * s,
            0.040f * s, 0.023f * s, RGB(219, 101, 71), 200u);
  }
  ART_ROUND(canvas, dx + 0.021f * s, dy, s, 1.0f, 0.0f, ink, {-0.081f, 0.047f},
            {-0.025f, 0.073f}, {0.028f, 0.064f}, {0.078f, 0.034f},
            {0.047f, 0.100f}, {0.005f, 0.122f}, {-0.04f, 0.105f});
  ellipse(canvas, dx + 0.027f * s, dy + 0.099f * s, 0.026f * s, 0.011f * s,
          RGB(239, 143, 107), 230u);
}

static void draw_shooter(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                         float s, uint32_t stage,
                         h2_garden_guard_plant_pose_t pose, bool head_only) {
  s *= pose.growth;
  const uint16_t ink = RGB(52, 93, 71);
  const uint16_t foliage = darken(RGB(96, 175, 100), stage * 40u);
  const float edge = fmaxf(0.55f, 0.017f * s);
  const float recoil = pose.action * pose.action;
  const float hx = gx + (-0.045f + pose.sway - recoil * 0.19f) * s;
  const float hy = gy - (0.54f + 0.045f * recoil) * s;
  if (!head_only) {
    ground_shadow(canvas, gx, gy, 0.34f * s);
    draw_leaf(canvas, gx, gy - 0.051f * s, s, -0.34f + 2.6f * pose.sway,
              foliage, ink);
    ocapsule(canvas, gx, gy - 0.048f * s, hx - 0.115f * s, hy + 0.18f * s,
             0.061f * s, edge, foliage, ink);
    draw_leaf(canvas, gx - 0.012f * s, gy - 0.075f * s, 0.90f * s,
              3.52f - 2.0f * pose.sway, RGB(130, 193, 110), ink);
  }
  draw_leaf(canvas, hx - 0.13f * s, hy - 0.15f * s, 0.74f * s,
            -2.13f + 5.0f * pose.sway + 0.29f * recoil, RGB(130, 191, 107),
            ink);
  // The trumpet grows from an upright, pear-shaped seed case. One flare
  // plane catches the light and the lower fold turns into the inner bell.
  ART_ROUND(canvas, hx, hy, s, 1.0f, 0.0f, ink, {-0.299f, 0.008f},
            {-0.244f, -0.162f}, {-0.133f, -0.228f}, {-0.005f, -0.199f},
            {0.104f, -0.192f}, {0.283f, -0.242f}, {0.388f, -0.195f},
            {0.435f, -0.075f}, {0.409f, 0.061f}, {0.328f, 0.116f},
            {0.18f, 0.076f}, {0.096f, 0.092f}, {0.020f, 0.236f},
            {-0.106f, 0.294f}, {-0.241f, 0.214f});
  ART_ROUND(canvas, hx, hy, s, 1.0f, 0.0f,
            darken(RGB(107, 176, 122), stage * 40u), {-0.271f, 0.014f},
            {-0.219f, -0.134f}, {-0.135f, -0.199f}, {-0.007f, -0.165f},
            {0.115f, -0.162f}, {0.277f, -0.211f}, {0.364f, -0.171f},
            {0.410f, -0.068f}, {0.384f, 0.044f}, {0.323f, 0.084f},
            {0.186f, 0.047f}, {0.081f, 0.066f}, {-0.006f, 0.204f},
            {-0.108f, 0.26f}, {-0.212f, 0.192f});
  ART_ROUND(canvas, hx, hy, s, 1.0f, 0.0f, RGB(162, 210, 128),
            {-0.238f, 0.011f}, {-0.194f, -0.112f}, {-0.124f, -0.175f},
            {-0.006f, -0.139f}, {0.141f, -0.135f}, {0.302f, -0.188f},
            {0.317f, -0.13f}, {0.136f, -0.056f}, {-0.020f, -0.075f},
            {-0.128f, -0.031f}, {-0.195f, 0.104f});
  ART_ROUND(canvas, hx, hy, s, 1.0f, 0.0f, RGB(73, 143, 113), {-0.025f, 0.127f},
            {0.085f, -0.003f}, {0.215f, -0.011f}, {0.308f, 0.009f},
            {0.322f, 0.065f}, {0.183f, 0.039f}, {0.065f, 0.075f});
  oellipse(canvas, hx + 0.326f * s, hy - 0.066f * s, 0.105f * s,
           (0.164f + 0.018f * recoil) * s, edge, RGB(215, 210, 119), ink);
  ellipse(canvas, hx + 0.342f * s, hy - 0.060f * s, 0.067f * s, 0.125f * s,
          recoil > 0.6f ? RGB(252, 229, 137) : RGB(48, 92, 74), 256u);
  ellipse(canvas, hx + 0.32f * s, hy - 0.005f * s, 0.038f * s, 0.065f * s,
          RGB(96, 142, 95), 190u);
  const float ex = hx - 0.113f * s, ey = hy - 0.018f * s;
  if (pose.blink) {
    capsule(canvas, ex - 0.051f * s, ey, ex + 0.054f * s, ey - 0.009f * s,
            0.013f * s, ink, 256u);
  } else {
    oellipse(canvas, ex, ey, 0.068f * s, 0.078f * s, edge, RGB(248, 239, 200),
             ink);
    ellipse(canvas, ex + 0.027f * s, ey - 0.003f * s, 0.033f * s, 0.049f * s,
            RGB(57, 79, 56), 256u);
    disc(canvas, ex + 0.018f * s, ey - 0.018f * s, 0.013f * s,
         RGB(255, 246, 211), 256u);
  }
  ART_ROUND(canvas, ex, ey, s, 1.0f, 0.0f, RGB(68, 112, 76), {-0.078f, -0.098f},
            {-0.035f, -0.134f}, {0.034f, -0.135f}, {0.062f, -0.101f},
            {0.015f, -0.11f}, {-0.037f, -0.106f});
  ellipse(canvas, ex - 0.045f * s, ey + 0.109f * s, 0.054f * s, 0.032f * s,
          RGB(189, 210, 129), 170u);
}

static void draw_pumpkin(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                         float s, uint32_t stage,
                         h2_garden_guard_plant_pose_t pose, bool head_only) {
  s *= pose.growth;
  const float squash = 1.0f - 0.11f * pose.impact;
  gx -= 0.075f * pose.impact * s;
  const float cy = gy - (0.295f * squash + pose.lift) * s;
  const float edge = fmaxf(0.72f, 0.025f * s);
  const uint16_t ink = RGB(119, 61, 39);
  const uint32_t flash = (uint32_t)(100.0f * pose.impact);
  if (!head_only) ground_shadow(canvas, gx, gy, 0.46f * s);
  const uint16_t rind = lighten(darken(RGB(215, 114, 47), stage * 32u), flash);
  const uint16_t front = lighten(darken(RGB(240, 145, 54), stage * 32u), flash);
  oellipse(canvas, gx - 0.196f * s, cy + 0.01f * s, 0.237f * s,
           0.253f * s * squash, edge, rind, ink);
  oellipse(canvas, gx + 0.189f * s, cy + 0.012f * s, 0.23f * s,
           0.252f * s * squash, edge, rind, ink);
  ellipse(canvas, gx, cy + 0.002f * s, 0.326f * s, 0.285f * s * squash,
          RGB(190, 89, 42), 256u);
  ellipse(canvas, gx - 0.033f * s, cy - 0.014f * s, 0.254f * s,
          0.273f * s * squash, front, 256u);
  ellipse(canvas, gx - 0.135f * s, cy - 0.075f * s, 0.105f * s,
          0.135f * s * squash, RGB(255, 182, 87), 170u);
  ellipse(canvas, gx - 0.315f * s, cy - 0.040f * s, 0.055f * s,
          0.13f * s * squash, RGB(251, 161, 69), 165u);
  ocapsule(canvas, gx, cy - 0.22f * s, gx + 0.066f * s, cy - 0.378f * s,
           0.050f * s, edge, RGB(131, 121, 61), RGB(72, 73, 45));
  draw_leaf(canvas, gx - 0.008f * s, cy - 0.25f * s, 0.60f * s,
            3.37f + 5.0f * pose.lift, RGB(105, 159, 71), RGB(60, 92, 50));
  const uint16_t face = RGB(82, 43, 34);
  const bool squint = pose.blink || pose.impact > 0.38f;
  for (int side = -1; side <= 1; side += 2) {
    const float x = gx + (float)side * 0.118f * s;
    if (squint) {
      capsule(canvas, x - 0.062f * s, cy - 0.025f * s, x + 0.062f * s,
              cy - 0.025f * s, 0.018f * s, face, 256u);
    } else {
      oellipse(canvas, x, cy - 0.038f * s, 0.052f * s, 0.060f * s, edge,
               RGB(250, 218, 143), face);
      disc(canvas, x + 0.014f * s, cy - 0.036f * s, 0.031f * s, face, 256u);
    }
    capsule(canvas, x + (float)side * 0.05f * s, cy - 0.132f * s,
            x - (float)side * 0.038f * s, cy - 0.092f * s, 0.023f * s, face,
            256u);
  }
  ellipse(canvas, gx, cy + 0.085f * s, 0.081f * s, 0.033f * s, face, 256u);
  if (stage >= 1u) {
    ART(canvas, gx, cy, s, 1.0f, 0.0f, RGB(144, 62, 34), {0.27f, -0.20f},
        {0.23f, -0.11f}, {0.28f, -0.058f}, {0.24f, 0.026f}, {0.35f, -0.072f},
        {0.28f, -0.118f}, {0.31f, -0.19f});
  }
  if (stage >= 2u) {
    ART(canvas, gx, cy, s, 1.0f, 0.0f, RGB(144, 62, 34), {-0.39f, 0.039f},
        {-0.26f, 0.085f}, {-0.243f, 0.158f}, {-0.143f, 0.221f},
        {-0.217f, 0.121f}, {-0.236f, 0.055f}, {-0.36f, 0.006f});
  }
}

static void draw_shovel(h2_garden_guard_canvas_t* canvas, float cx, float cy,
                        float s) {
  capsule(canvas, cx - 0.3f * s, cy + 0.3f * s, cx + 0.08f * s, cy - 0.08f * s,
          0.06f * s, RGB(132, 90, 48), 256u);
  capsule(canvas, cx - 0.36f * s, cy + 0.24f * s, cx - 0.24f * s,
          cy + 0.36f * s, 0.07f * s, RGB(96, 64, 34), 256u);
  disc(canvas, cx + 0.16f * s, cy - 0.16f * s, 0.2f * s, RGB(176, 184, 196),
       256u);
  capsule(canvas, cx + 0.06f * s, cy - 0.06f * s, cx + 0.3f * s, cy - 0.3f * s,
          0.13f * s, RGB(176, 184, 196), 256u);
  disc(canvas, cx + 0.12f * s, cy - 0.2f * s, 0.07f * s, RGB(230, 236, 244),
       180u);
}

static const h2_garden_guard_plant_pose_t k_neutral_plant = {.growth = 1.0f};

static void draw_frost(h2_garden_guard_canvas_t* canvas, float x, float y,
                       float size, uint32_t stage,
                       h2_garden_guard_plant_pose_t pose, bool head_only) {
  const float s = size * pose.growth;
  const float recoil = pose.action * pose.action;
  const float hx = x + (pose.sway - 0.16f * recoil) * s;
  const float hy = y - (0.56f + 0.03f * recoil) * s;
  const uint16_t ink = RGB(51, 103, 114);
  const uint16_t leaf = darken(RGB(106, 176, 160), stage * 38u);
  if (!head_only) {
    ground_shadow(canvas, x, y, 0.34f * s);
    draw_leaf(canvas, x, y - 0.053f * s, s, -0.58f, leaf, ink);
    ocapsule(canvas, x, y - 0.04f * s, hx - 0.04f * s, hy + 0.18f * s,
             0.061f * s, 0.65f, leaf, ink);
    draw_leaf(canvas, x, y - 0.11f * s, s * 0.88f, 3.43f + pose.sway,
              RGB(151, 199, 166), ink);
  }
  const uint16_t petal = darken(RGB(146, 213, 225), stage * 33u);
  ART_ROUND(canvas, hx, hy, s, 1.0f, 0.0f, ink, {-0.28f, 0.025f},
            {-0.26f, -0.18f}, {-0.14f, -0.159f}, {-0.087f, -0.31f},
            {0.059f, -0.167f}, {0.15f, -0.25f}, {0.211f, -0.114f},
            {0.393f, -0.117f}, {0.422f, 0.10f}, {0.162f, 0.145f},
            {-0.059f, 0.231f}, {-0.219f, 0.174f});
  ART_ROUND(canvas, hx, hy, s, 1.0f, 0.0f, petal, {-0.25f, 0.023f},
            {-0.23f, -0.142f}, {-0.133f, -0.128f}, {-0.082f, -0.264f},
            {0.053f, -0.121f}, {0.138f, -0.207f}, {0.188f, -0.074f},
            {0.36f, -0.078f}, {0.386f, 0.076f}, {0.14f, 0.121f},
            {-0.06f, 0.201f}, {-0.192f, 0.15f});
  oellipse(canvas, hx + 0.336f * s, hy - 0.012f * s, 0.087f * s, 0.131f * s,
           0.7f, RGB(218, 240, 220), ink);
  ellipse(canvas, hx + 0.35f * s, hy - 0.012f * s, 0.047f * s, 0.086f * s,
          recoil > 0.55f ? RGB(239, 253, 219) : RGB(67, 137, 166), 256u);
  ellipse(canvas, hx - 0.031f * s, hy - 0.032f * s, 0.067f * s,
          (pose.blink ? 0.02f : 0.067f) * s, RGB(247, 247, 216), 256u);
  ellipse(canvas, hx - 0.009f * s, hy - 0.027f * s, 0.030f * s,
          (pose.blink ? 0.012f : 0.040f) * s, ink, 256u);
  ellipse(canvas, hx - 0.109f * s, hy + 0.072f * s, 0.034f * s, 0.022f * s,
          RGB(232, 162, 180), 225u);
}

static void draw_bramble(h2_garden_guard_canvas_t* canvas, float x, float y,
                         float size, uint32_t stage,
                         h2_garden_guard_plant_pose_t pose, bool head_only) {
  const float s = size * pose.growth;
  x -= 0.075f * s * pose.impact;
  const float cy = y - (0.30f + pose.lift - 0.04f * pose.impact) * s;
  const uint16_t ink = RGB(105, 69, 102);
  const uint16_t berry = lighten(darken(RGB(172, 89, 132), stage * 45u),
                                 (uint32_t)(75.0f * pose.impact));
  if (!head_only) ground_shadow(canvas, x, y, 0.42f * s);
  draw_leaf(canvas, x, cy + 0.145f * s, s, 3.55f, RGB(116, 166, 92),
            RGB(58, 101, 72));
  draw_leaf(canvas, x, cy + 0.13f * s, s, -0.3f, RGB(108, 153, 86),
            RGB(58, 101, 72));
  ART_ROUND(canvas, x, cy, s, 1.0f, 0.0f, ink, {-0.38f, 0.097f},
            {-0.33f, -0.106f}, {-0.42f, -0.206f}, {-0.24f, -0.17f},
            {-0.21f, -0.343f}, {-0.055f, -0.241f}, {0.085f, -0.39f},
            {0.133f, -0.218f}, {0.3f, -0.306f}, {0.286f, -0.123f},
            {0.436f, -0.138f}, {0.348f, 0.055f}, {0.237f, 0.225f},
            {-0.199f, 0.254f});
  oellipse(canvas, x + 0.13f * s, cy - 0.02f * s, 0.202f * s, 0.214f * s, 0.64f,
           berry, ink);
  oellipse(canvas, x - 0.075f * s, cy, 0.256f * s, 0.224f * s, 0.66f,
           lighten(berry, 40u), ink);
  ellipse(canvas, x - 0.181f * s, cy - 0.078f * s, 0.078f * s, 0.095f * s,
          RGB(218, 142, 160), 180u);
  for (int side = -1; side <= 1; side += 2) {
    const float eye_x = x + (float)side * 0.088f * s;
    ellipse(canvas, eye_x, cy - 0.026f * s, 0.042f * s,
            (pose.blink ? 0.017f : 0.057f) * s, RGB(250, 224, 171), 256u);
    disc(canvas, eye_x + 0.011f * s, cy - 0.023f * s, 0.027f * s, ink, 256u);
  }
  ellipse(canvas, x + 0.012f * s, cy + 0.09f * s, 0.072f * s, 0.025f * s,
          RGB(104, 59, 87), 256u);
}

static void draw_plant_kind(h2_garden_guard_canvas_t* canvas, uint8_t kind,
                            float gx, float gy, float s, uint32_t stage,
                            h2_garden_guard_plant_pose_t pose, bool head_only) {
  switch (kind) {
    case H2_GARDEN_GUARD_PLANT_SUNFLOWER:
      draw_sunflower(canvas, gx, gy, s, stage, pose, head_only);
      break;
    case H2_GARDEN_GUARD_PLANT_SHOOTER:
      draw_shooter(canvas, gx, gy, s, stage, pose, head_only);
      break;
    case H2_GARDEN_GUARD_PLANT_PUMPKIN:
      draw_pumpkin(canvas, gx, gy, s, stage, pose, head_only);
      break;
    case H2_GARDEN_GUARD_PLANT_FROST:
      draw_frost(canvas, gx, gy, s, stage, pose, head_only);
      break;
    case H2_GARDEN_GUARD_PLANT_BRAMBLE:
      draw_bramble(canvas, gx, gy, s, stage, pose, head_only);
      break;
    default:
      break;
  }
}

/* Side view of a beetle walking left; t is its animation clock in seconds. */
/* Packed world cels and journal portraits share the same original art. */
typedef struct zombie_frame {
  uint16_t offset, duration_ms;
  uint8_t left, top, right, bottom;
} zombie_frame_t;

typedef struct zombie_sprite {
  const uint16_t* palette;
  const uint8_t* pixels;
  const zombie_frame_t* frames;
} zombie_sprite_t;

#include "h2_garden_guard_zombies.inc"

static const zombie_sprite_t k_zombies[] = {
    {k_gardener_palette, k_gardener_pixels, k_gardener_frames},
    {k_armored_palette, k_armored_pixels, k_armored_frames},
    {k_runner_palette, k_runner_pixels, k_runner_frames},
};

enum {
  ZOMBIE_ANCHOR_X = 32,
  ZOMBIE_ANCHOR_Y = 64,
  ZOMBIE_CHEW_FIRST = 8,
  ZOMBIE_CHEW_CONTACT = 11,
  ZOMBIE_HIT = 13,
  ZOMBIE_DEFEAT = 14,
  ZOMBIE_PORTRAIT = 16,
};

static uint16_t zombie_color(uint16_t color, bool broken_plate) {
  // Scrape the blue glaze when the shellback loses its shoulder protection.
  const unsigned blue = color & 31u;
  const unsigned red = color >> 11;
  if (broken_plate && blue >= red + 4u)
    return blend(color, RGB(140, 103, 79), 156u);
  return color;
}

static void draw_zombie_cel(h2_garden_guard_canvas_t* canvas,
                            const zombie_sprite_t* sprite, unsigned cel,
                            float x, float y, float scale, uint32_t opacity,
                            bool broken_plate) {
  const zombie_frame_t* frame = &sprite->frames[cel];
  region_t bounds;
  if (!region_begin(canvas, x + frame->left * scale, y + frame->top * scale,
                    x + frame->right * scale, y + frame->bottom * scale,
                    &bounds))
    return;
  const float inv = 1.0f / scale;
  const uint8_t* data = sprite->pixels + frame->offset;
  int32_t source_row = -1;
  const uint8_t* run = data;
  int32_t begin = 0, end = 0;
  for (int32_t py = bounds.y0; py < bounds.y1; ++py) {
    const int32_t sy = (int32_t)floorf(((float)py + 0.5f - y) * inv);
    if (sy < frame->top || sy >= frame->bottom) continue;
    while (source_row < sy) {
      begin = *data++;
      const uint8_t length = *data++;
      end = begin + length;
      run = data;
      data += length;
      ++source_row;
    }
    if (begin == end) continue;
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    const int32_t left =
        (int32_t)fmaxf((float)bounds.x0, floorf(x + (float)begin * scale));
    const int32_t right =
        (int32_t)fminf((float)bounds.x1, ceilf(x + (float)end * scale));
    for (int32_t px = left; px < right; ++px) {
      const int32_t sx = (int32_t)floorf(((float)px + 0.5f - x) * inv);
      if (sx < begin || sx >= end) continue;
      const uint8_t sample = run[sx - begin];
      if (sample == 0u) continue;
      const uint32_t alpha = ((uint32_t)sample >> 6) * opacity / 3u;
      plot(canvas, row, depth, px,
           zombie_color(sprite->palette[sample & 63u], broken_plate), alpha);
    }
  }
}

static unsigned timed_zombie_cel(const zombie_sprite_t* sprite, unsigned first,
                                 unsigned end, uint32_t phase) {
  for (unsigned i = first; i + 1u < end; ++i) {
    if (phase < sprite->frames[i].duration_ms) return i;
    phase -= sprite->frames[i].duration_ms;
  }
  return end - 1u;
}

static unsigned chew_cel(const zombie_sprite_t* sprite, uint32_t phase) {
  uint32_t lead = 0, recovery = 0;
  for (unsigned i = ZOMBIE_CHEW_FIRST; i < ZOMBIE_CHEW_CONTACT; ++i)
    lead += sprite->frames[i].duration_ms;
  for (unsigned i = ZOMBIE_CHEW_CONTACT; i < ZOMBIE_HIT; ++i)
    recovery += sprite->frames[i].duration_ms;
  /* Map the authored timing onto the shared bite clock. The first closed
   * jaw cel begins on the same tick as the contact and thorn recoil. */
  const uint32_t hit = H2_GARDEN_GUARD_PEST_BITE_CONTACT_MS;
  if (phase < hit)
    return timed_zombie_cel(sprite, ZOMBIE_CHEW_FIRST, ZOMBIE_CHEW_CONTACT,
                            phase * lead / hit);
  return timed_zombie_cel(
      sprite, ZOMBIE_CHEW_CONTACT, ZOMBIE_HIT,
      (phase - hit) * recovery / (H2_GARDEN_GUARD_PEST_BITE_MS - hit));
}

static void draw_zombie(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                        float size, const h2_garden_guard_pest_t* pest) {
  if (pest->kind >= H2_GARDEN_GUARD_PEST_KIND_COUNT) return;
  const zombie_sprite_t* sprite = &k_zombies[pest->kind];
  const float scale = size / 45.0f;
  unsigned cel;
  uint32_t opacity = 256u;
  if (pest->exit_ms != 0u) {
    const uint32_t age = H2_GARDEN_GUARD_PEST_EXIT_MS - pest->exit_ms;
    cel = timed_zombie_cel(sprite, ZOMBIE_DEFEAT, ZOMBIE_PORTRAIT, age);
    const uint32_t fade_ms = H2_GARDEN_GUARD_PEST_EXIT_MS -
                             sprite->frames[ZOMBIE_DEFEAT].duration_ms -
                             sprite->frames[ZOMBIE_PORTRAIT - 1].duration_ms;
    if (pest->exit_ms < fade_ms)
      opacity = (uint32_t)pest->exit_ms * 256u / fade_ms;
  } else if (pest->flash_ms != 0u) {
    cel = ZOMBIE_HIT;
  } else if (pest->eating) {
    cel = chew_cel(sprite, pest->chew_ms);
  } else {
    cel = timed_zombie_cel(sprite, 0u, ZOMBIE_CHEW_FIRST, pest->stride_ms);
  }
  ellipse(canvas, gx + 0.08f * size, gy, size * 0.31f, size * 0.093f,
          RGB(18, 40, 14), opacity * 104u / 256u);
  draw_zombie_cel(
      canvas, sprite, cel, gx - ZOMBIE_ANCHOR_X * scale,
      gy - ZOMBIE_ANCHOR_Y * scale, scale, opacity,
      pest->kind == H2_GARDEN_GUARD_PEST_SHELLBACK && pest->armor <= 0);
}

void h2_garden_guard_render_zombie_portrait(h2_garden_guard_canvas_t* canvas,
                                            h2_garden_guard_pest_kind_t kind,
                                            float x, float ground_y,
                                            float size) {
  if ((unsigned)kind >= H2_GARDEN_GUARD_PEST_KIND_COUNT) return;
  const float scale = size / 52.0f;
  draw_zombie_cel(canvas, &k_zombies[kind], ZOMBIE_PORTRAIT, x - 32.0f * scale,
                  ground_y - 62.0f * scale, scale, 256u, false);
}

static void draw_cart(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                      float s) {
  const float line = rim_width(s);
  ground_shadow(canvas, gx, gy, 0.38f * s);
  ocapsule(canvas, gx - 0.24f * s, gy - 0.36f * s, gx - 0.44f * s,
           gy - 0.62f * s, 0.03f * s, line, RGB(120, 84, 50), RGB(60, 40, 22));
  capsule(canvas, gx - 0.52f * s, gy - 0.63f * s, gx - 0.38f * s,
          gy - 0.61f * s, 0.045f * s, RGB(40, 40, 46), 256u);
  rounded_rect(canvas, gx + 0.25f * s, gy - 0.22f * s, gx + 0.37f * s,
               gy - 0.02f * s, 0.03f * s, RGB(120, 86, 30), 256u);
  rounded_rect(canvas, gx + 0.26f * s, gy - 0.21f * s, gx + 0.36f * s,
               gy - 0.03f * s, 0.025f * s, RGB(236, 196, 84), 256u);
  rounded_rect(canvas, gx - 0.3f * s - line, gy - 0.46f * s - line,
               gx + 0.28f * s + line, gy - 0.14f * s + line, 0.08f * s + line,
               RGB(110, 22, 18), 256u);
  rounded_rect(canvas, gx - 0.3f * s, gy - 0.46f * s, gx + 0.28f * s,
               gy - 0.14f * s, 0.08f * s, RGB(206, 50, 38), 256u);
  rounded_rect(canvas, gx - 0.24f * s, gy - 0.41f * s, gx + 0.22f * s,
               gy - 0.35f * s, 0.03f * s, RGB(240, 110, 92), 256u);
  odisc(canvas, gx - 0.17f * s, gy - 0.1f * s, 0.1f * s, line, RGB(40, 40, 46),
        RGB(14, 14, 16));
  odisc(canvas, gx + 0.14f * s, gy - 0.1f * s, 0.1f * s, line, RGB(40, 40, 46),
        RGB(14, 14, 16));
  disc(canvas, gx - 0.17f * s, gy - 0.1f * s, 0.04f * s, RGB(186, 186, 196),
       256u);
  disc(canvas, gx + 0.14f * s, gy - 0.1f * s, 0.04f * s, RGB(186, 186, 196),
       256u);
}

static void draw_sun(h2_garden_guard_canvas_t* canvas, float x, float y,
                     float r, float spin, uint32_t alpha) {
  disc(canvas, x, y, r * 1.45f, RGB(255, 226, 96), alpha * 70u / 256u);
  for (unsigned k = 0; k < 8u; ++k) {
    const float angle = spin + (float)k * (H2_GARDEN_GUARD_PI / 4.0f);
    const float c = cosf(angle), s = sinf(angle);
    capsule(canvas, x + c * r * 0.95f, y + s * r * 0.95f, x + c * r * 1.3f,
            y + s * r * 1.3f, r * 0.11f, RGB(255, 196, 36), alpha);
  }
  disc(canvas, x, y, r, RGB(255, 200, 34), alpha);
  disc(canvas, x, y, r * 0.72f, RGB(255, 224, 88), alpha);
  disc(canvas, x - r * 0.28f, y - r * 0.28f, r * 0.26f, RGB(255, 248, 196),
       alpha * 160u / 256u);
}

/* ---- Courtyard background ---------------------------------------------- */

static float row_of_scale(const h2_garden_guard_layout_t* l, float t) {
  return l->far_y +
         (l->near_y - l->far_y) * (t - l->far_ratio) / (1.0f - l->far_ratio);
}

static uint16_t wood_color(float x, float y, float plank, uint32_t noise) {
  const int32_t index = (int32_t)(y / plank);
  const float grain =
      sinf(x * 0.045f + (float)index * 1.7f + 2.0f * sinf(x * 0.011f));
  const float k =
      1.0f + 0.07f * grain + 0.04f * ((float)(noise & 15u) / 15.0f - 0.5f);
  return scale565(RGB(39, 72, 69), k);
}

/* Front face of the raised lawn bed below its near edge. */
static uint16_t facade_color(const h2_garden_guard_layout_t* l, float x,
                             float y, uint32_t noise) {
  const float depth = y - l->near_y;
  const float height = (float)l->height - l->near_y;
  const float lawn_x0 = l->mid_x - 0.5f * l->near_w;
  const float lawn_x1 = l->mid_x + 0.5f * l->near_w;
  const float lip = fmaxf(2.0f, 3.0f * l->unit);
  if (depth < 1.0f) return RGB(221, 200, 150);
  if (depth < lip)
    return x >= lawn_x0 && x < lawn_x1 ? RGB(86, 128, 68) : RGB(219, 192, 144);
  const float course = fmaxf((height - lip) * 0.5f, 4.0f);
  const float block = fmaxf(l->near_w * 0.075f, 8.0f);
  const int32_t row = (int32_t)((depth - lip) / course);
  const float shifted = x + ((row & 1) ? block * 0.5f : 0.0f);
  const int32_t brick = (int32_t)floorf(shifted / block);
  const uint32_t tone =
      hash32((uint32_t)row * 2654435761u ^ (uint32_t)brick * 97u) & 31u;
  uint16_t color = RGB(154u + tone / 2u, 124u + tone / 2u, 88u + tone / 3u);
  if (fmodf(depth - lip, course) < 1.2f ||
      fractf(shifted / block) * block < 1.2f)
    color = RGB(86, 73, 56);
  else if (fmodf(depth - lip, course) < 2.4f)
    color = lighten(color, 55u);
  else if ((noise & 63u) < 2u)
    color = darken(color, 40u);
  return scale565(color, 1.0f - 0.32f * (depth / fmaxf(height, 1.0f)));
}

/* House wall running back along the patio: siding, two windows and a roof
 * edge. hgt is the height above the wall's ground line in lawn widths and
 * fw the depth along the wall. Returns false above the roof. */
static bool house_color(float hgt, float fw, float px_per_unit, uint32_t noise,
                        uint16_t* out) {
  if (hgt > WALL_HEIGHT + ROOF_HEIGHT) return false;
  if (hgt > WALL_HEIGHT) {
    const float tiles = (hgt - WALL_HEIGHT) / 0.025f;
    uint16_t c = RGB(62, 108, 105);
    if (seam_px(tiles, 0.025f * px_per_unit) < 0.7f) c = RGB(43, 74, 72);
    if (hgt < WALL_HEIGHT + 0.012f) c = RGB(36, 59, 56);
    *out = scale565(c, 0.9f + 0.2f * (float)(noise & 7u) / 7.0f);
    return true;
  }
  if (hgt < 0.016f) {
    *out = RGB(132, 120, 104);
    return true;
  }
  const bool window =
      hgt > 0.1f && hgt < 0.21f &&
      ((fw > 0.16f && fw < 0.38f) || (fw > 0.62f && fw < 0.84f));
  if (window) {
    const float local = fw < 0.5f ? (fw - 0.16f) / 0.22f : (fw - 0.62f) / 0.22f;
    const float hv = (hgt - 0.1f) / 0.11f;
    if (local < 0.08f || local > 0.92f || hv < 0.1f || hv > 0.9f ||
        fabsf(local - 0.5f) < 0.03f || fabsf(hv - 0.5f) < 0.04f)
      *out = RGB(243, 213, 164);
    else
      *out = fractf((local + hv) * 1.3f) < 0.18f ? RGB(174, 214, 195)
                                                 : RGB(94, 155, 154);
    return true;
  }
  uint16_t c = RGB(214, 196, 160);
  if (seam_px(hgt / 0.024f, 0.024f * px_per_unit) < 0.6f)
    c = RGB(176, 158, 124);
  *out = c;
  return true;
}

static uint16_t lawn_color(uint32_t lane, uint32_t col, float f,
                           float lane_seam, float cell_seam, float u,
                           uint32_t noise) {
  const bool odd = ((lane + col) & 1u) != 0u;
  uint16_t color = odd ? RGB(108, 161, 89) : RGB(121, 174, 94);
  float k =
      0.99f + 0.03f * (float)(hash32(lane * 31u + col * 7u + 3u) & 15u) / 15.0f;
  /* Farther grass is a little darker and cooler. */
  k *= 1.0f - 0.14f * f;
  // Soft house shade stops short of the second column.
  k *= 1.0f - 0.16f * clampf((0.20f - (u + 0.09f * f)) * 8.0f, 0.0f, 1.0f);
  const uint32_t speck = noise & 63u;
  if (speck == 0u)
    k *= 0.91f;
  else if (speck == 1u)
    k *= 1.075f;
  if (lane_seam < 0.9f)
    k *= 0.85f;
  else if (cell_seam < 0.7f)
    k *= 0.92f;
  return scale565(color, k);
}

static uint16_t sky_color(float y, float top, float horizon) {
  const float s = clampf((y - top) / fmaxf(horizon - top, 1.0f), 0.0f, 1.0f);
  return blend(RGB(104, 171, 181), RGB(240, 226, 175), (uint32_t)(s * 256.0f));
}

static void draw_courtyard_details(h2_garden_guard_canvas_t* canvas,
                                   const h2_garden_guard_layout_t* l) {
  const float t_bed = h2_garden_guard_depth_scale(l, 1.0f + BED_DEPTH);
  const float y_fence = row_of_scale(l, t_bed);
  const float hedge_low =
      y_fence - (FENCE_HEIGHT + HEDGE_HEIGHT - 0.02f) * l->near_w * t_bed;
  canvas->clip_y0 = (int32_t)ceilf(l->bar_h);
  /* Distant tree crowns rise above the hedge where there is sky. */
  canvas->clip_y1 = (int32_t)hedge_low;
  if (hedge_low - l->bar_h > 12.0f) {
    const float t_tree = h2_garden_guard_depth_scale(l, 1.9f);
    const float y_tree = row_of_scale(l, t_tree);
    static const float trees[][2] = {
        {-0.28f, 0.2f}, {0.08f, 0.26f}, {0.42f, 0.18f},
        {0.74f, 0.24f}, {1.06f, 0.2f},  {1.3f, 0.22f},
    };
    for (unsigned i = 0; i < sizeof(trees) / sizeof(trees[0]); ++i) {
      const float x = l->mid_x + (trees[i][0] - 0.5f) * l->near_w * t_tree;
      const float r = trees[i][1] * 0.5f * l->near_w * t_tree * 0.55f;
      const float y = y_tree - trees[i][1] * l->near_w * t_tree;
      disc(canvas, x - r * 0.6f, y + r * 0.3f, r * 0.8f, RGB(52, 104, 56),
           256u);
      disc(canvas, x + r * 0.6f, y + r * 0.35f, r * 0.75f, RGB(48, 98, 52),
           256u);
      disc(canvas, x, y, r, RGB(62, 120, 62), 256u);
      disc(canvas, x - r * 0.3f, y - r * 0.35f, r * 0.45f, RGB(86, 146, 76),
           200u);
    }
    /* Small streaks of cloud read as a separate distant plane. */
    const float sky = hedge_low - l->bar_h;
    if (sky > 60.0f) {
      const float radius = fminf(sky * 0.16f, 31.0f * l->unit);
      const float x_sun = (float)l->width * 0.64f;
      const float y_sun = l->bar_h + sky * 0.37f;
      disc(canvas, x_sun, y_sun, radius * 1.22f, RGB(252, 225, 154), 88u);
      disc(canvas, x_sun, y_sun, radius, RGB(255, 228, 157), 246u);
      static const float clouds[][2] = {
          {0.18f, 0.22f}, {0.82f, 0.12f}, {0.42f, 0.49f}};
      for (unsigned i = 0; i < sizeof(clouds) / sizeof(clouds[0]); ++i) {
        const float x = (float)l->width * clouds[i][0];
        const float y = l->bar_h + sky * clouds[i][1] + 10.0f * l->unit;
        const float r = 16.0f * l->unit;
        ellipse(canvas, x, y + r * 0.15f, r * 2.7f, r * 0.40f,
                RGB(249, 236, 205), 200u);
        ellipse(canvas, x - r * 0.65f, y - r * 0.16f, r * 1.0f, r * 0.48f,
                RGB(254, 242, 211), 225u);
      }
    }
  }
  canvas->clip_y1 = 0;

  /* Shrubs along the fence and flowers in the bed, back to front. */
  const float t_shrub =
      h2_garden_guard_depth_scale(l, 1.0f + BED_DEPTH - 0.02f);
  for (unsigned i = 0; i < 11u; ++i) {
    const float u = -0.18f + (float)i * 0.135f;
    float x, y;
    h2_garden_guard_ground_point(l, u, 1.0f + BED_DEPTH - 0.02f, &x, &y);
    const float r = 0.03f * l->near_w * t_shrub;
    disc(canvas, x - r * 0.7f, y - r * 0.6f, r * 0.8f, RGB(44, 98, 44), 256u);
    disc(canvas, x + r * 0.7f, y - r * 0.6f, r * 0.8f, RGB(40, 92, 40), 256u);
    disc(canvas, x, y - r * 1.0f, r, RGB(58, 118, 54), 256u);
    disc(canvas, x - r * 0.3f, y - r * 1.4f, r * 0.4f, RGB(96, 158, 80), 180u);
  }
  static const uint16_t blooms[4] = {RGB(206, 118, 124), RGB(236, 225, 193),
                                     RGB(222, 171, 81), RGB(161, 140, 166)};
  for (unsigned i = 0; i < 24u; ++i) {
    const uint32_t h = hash32(i * 2654435761u + 11u);
    const float u =
        -0.12f + 1.23f * ((float)i + (float)(h & 255u) / 512.0f) / 24.0f;
    const float f =
        1.035f + (BED_DEPTH - 0.07f) * (float)((h >> 8) & 255u) / 255.0f;
    float x, y;
    h2_garden_guard_ground_point(l, u, f, &x, &y);
    const float r =
        fmaxf(0.009f * l->near_w * h2_garden_guard_depth_scale(l, f), 1.2f);
    capsule(canvas, x, y, x, y - r * 2.2f, r * 0.3f, RGB(56, 116, 46), 256u);
    const uint16_t c = blooms[(h >> 16) & 3u];
    for (unsigned k = 0; k < 5u; ++k) {
      const float a = (float)k * (2.0f * H2_GARDEN_GUARD_PI / 5.0f);
      disc(canvas, x + cosf(a) * r * 0.7f, y - r * 2.4f + sinf(a) * r * 0.7f,
           r * 0.55f, c, 256u);
    }
    disc(canvas, x, y - r * 2.4f, r * 0.4f, RGB(250, 200, 40), 256u);
  }
  // Terracotta pots set against the back border, outside every playable tile.
  for (unsigned i = 0; i < 3u; ++i) {
    float x, y;
    h2_garden_guard_ground_point(l, 0.21f + (float)i * 0.305f, 1.09f, &x, &y);
    const float s = 0.047f * l->near_w;
    ground_shadow(canvas, x, y, 0.45f * s);
    trapezoid(canvas, y - 0.62f * s, x - 0.31f * s, x + 0.31f * s, y,
              x - 0.23f * s, x + 0.23f * s, RGB(157, 88, 66), 256u);
    ellipse(canvas, x, y - 0.59f * s, 0.35f * s, 0.12f * s, RGB(218, 142, 101),
            256u);
    ellipse(canvas, x + 0.04f * s, y - 0.77f * s, 0.32f * s, 0.23f * s,
            RGB(65, 108, 67), 256u);
    ellipse(canvas, x - 0.2f * s, y - 0.85f * s, 0.21f * s, 0.12f * s,
            RGB(117, 158, 95), 256u);
    disc(canvas, x + 0.10f * s, y - 0.98f * s, 0.105f * s, RGB(237, 205, 137),
         256u);
  }
  canvas->clip_y0 = 0;
}

void h2_garden_guard_render_background(h2_garden_guard_canvas_t* canvas,
                                       const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  region_t g;
  if (!region_begin(canvas, 0.0f, 0.0f, (float)canvas->width,
                    (float)canvas->height, &g))
    return;
  const float r = l->far_ratio, a = 1.0f / r - 1.0f;
  const float plank = fmaxf(l->bar_h * 0.5f, 4.0f);
  const float t_bed = h2_garden_guard_depth_scale(l, 1.0f + BED_DEPTH);
  const float y_fence = row_of_scale(l, t_bed);
  const float fence_px = l->near_w * t_bed;
  const float rows_per_t = (l->near_y - l->far_y) / (1.0f - r);
  for (int32_t y = g.y0; y < g.y1; ++y) {
    uint16_t* row = row_at(canvas, y);
    const float fy = (float)y + 0.5f;
    const float t = r + (fy - l->far_y) / rows_per_t;
    const float f = t > 0.0f ? (1.0f / t - 1.0f) / a : 1e9f;
    /* Screen pixels per lane and per column on this row's ground. */
    const float lane_px = rows_per_t * a * t * t / (float)LANES;
    const float col_px = l->near_w * t / (float)COLS;
    for (int32_t x = g.x0; x < g.x1; ++x) {
      const float fx = (float)x + 0.5f;
      const uint32_t noise =
          hash32((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ 0x5A17u);
      /* MSVC cannot infer that house_color assigns its out parameter whenever
       * it returns true. Keep initialization explicit before the done paths. */
      uint16_t color = RGB(105, 79, 56);
      if (fy < l->bar_h) {
        /* Toolbar plank with a dark lower edge. */
        color = wood_color(fx, fy, plank, noise);
        if (fy > l->bar_h - 3.0f || fmodf(fy, plank) < 1.0f)
          color = RGB(22, 46, 48);
        else if (fy > l->bar_h - 5.0f)
          color = lighten(color, 40u);
        row[x] = color;
        continue;
      }
      if (fy >= l->near_y) {
        row[x] = facade_color(l, fx, fy, noise);
        continue;
      }
      const float u =
          t > 0.0f ? 0.5f + (fx - l->mid_x) / (l->near_w * t) : -9.0f;
      bool done = false;
      if (u < WALL_U) {
        /* The ray meets the house wall before the ground behind it. */
        const float tw = (fx - l->mid_x) / ((WALL_U - 0.5f) * l->near_w);
        if (tw > 0.0f) {
          const float px_per_unit = l->near_w * tw;
          const float hgt = (row_of_scale(l, tw) - fy) / px_per_unit;
          const float fw = (1.0f / tw - 1.0f) / a;
          /* The house ends where the back fence starts. */
          if (fw <= 1.0f + BED_DEPTH)
            done = house_color(hgt, fw, px_per_unit, noise, &color);
        }
      }
      if (!done && f > 1.0f + BED_DEPTH) {
        /* Beyond the bed: the back fence, the hedge above it, then sky. */
        const float hgt = (y_fence - fy) / fence_px;
        const float uf = 0.5f + (fx - l->mid_x) / fence_px;
        if (hgt <= FENCE_HEIGHT) {
          const float board = uf / 0.03f;
          color = scale565(
              RGB(127, 148, 111),
              0.92f +
                  0.12f *
                      (float)(hash32((uint32_t)(int32_t)floorf(board) + 77u) &
                              15u) /
                      15.0f);
          if (seam_px(board, 0.03f * fence_px) < 0.8f) color = RGB(69, 101, 86);
          if (hgt > FENCE_HEIGHT - 0.01f) color = RGB(186, 184, 139);
          if (fabsf(hgt - 0.022f) < 0.005f || fabsf(hgt - 0.05f) < 0.005f)
            color = darken(color, 50u);
        } else {
          const float top = FENCE_HEIGHT + HEDGE_HEIGHT +
                            0.012f * sinf(uf * 31.0f) +
                            0.008f * sinf(uf * 77.0f + 1.3f);
          if (hgt <= top) {
            const uint32_t blob = hash32((uint32_t)(x / 3) * 2246822519u ^
                                         (uint32_t)(y / 3) * 3266489917u);
            color = scale565(RGB(56, 112, 50),
                             0.78f + 0.4f * (float)(blob & 15u) / 15.0f);
          } else {
            color = sky_color(fy, l->bar_h, y_fence);
          }
        }
        done = true;
      }
      if (!done) {
        if (f > 1.0f) {
          color = f < 1.0f + CURB * 1.5f
                      ? RGB(211, 183, 135)
                      : scale565(RGB(105, 79, 56),
                                 0.92f + 0.16f * (float)(noise & 15u) / 15.0f);
        } else if (u >= 0.0f && u < 1.0f) {
          const uint32_t lane = (uint32_t)clampf((1.0f - f) * (float)LANES,
                                                 0.0f, (float)LANES - 1.0f);
          const uint32_t col =
              (uint32_t)clampf(u * (float)COLS, 0.0f, (float)COLS - 1.0f);
          const float cell_seam = seam_px(u * (float)COLS, col_px);
          const float lane_seam = seam_px((1.0f - f) * (float)LANES, lane_px);
          color = lawn_color(lane, col, f, lane_seam, cell_seam, u, noise);
        } else if (u < 0.0f && u >= -CURB) {
          color = RGB(217, 190, 145);
        } else if (u > 1.0f && u <= 1.0f + CURB) {
          color = RGB(217, 190, 145);
        } else if (u < 0.0f) {
          /* Patio flagstones. */
          const float tu = u / 0.045f, tf = f * 8.0f;
          const uint32_t tone = hash32((uint32_t)(int32_t)floorf(tu) * 131u ^
                                       (uint32_t)(int32_t)floorf(tf) * 977u) &
                                31u;
          color = RGB(184u + tone / 2u, 166u + tone / 2u, 134u + tone / 3u);
          if (seam_px(tu, 0.045f * l->near_w * t) < 0.8f ||
              seam_px(tf, lane_px * (float)LANES / 8.0f) < 0.8f)
            color = RGB(128, 112, 88);
        } else {
          /* Clay pavers stagger at the entry, in the same ground projection. */
          const float course = f * 16.0f;
          const float brick =
              (u - 1.0f) * 14.0f + (((int32_t)course & 1) ? 0.5f : 0.0f);
          const float mortar =
              fminf(seam_px(brick, l->near_w * t / 14.0f),
                    seam_px(course, lane_px * (float)LANES / 16.0f));
          const uint32_t tone = hash32((uint32_t)(int32_t)course * 97u ^
                                       (uint32_t)(int32_t)brick * 73u) &
                                15u;
          color = mortar < 0.7f ? RGB(129, 111, 83)
                                : RGB(175u + tone, 137u + tone, 104u + tone);
          if (mortar >= 0.7f && mortar < 1.4f) color = lighten(color, 28u);
        }
      }
      row[x] = color;
    }
  }
  if (canvas->measure || canvas->pixels == NULL) return;
  draw_courtyard_details(canvas, l);
  /* The toolbar casts a soft shadow onto the courtyard. */
  const int32_t shadow = (int32_t)fmaxf(4.0f, 7.0f * l->unit);
  for (int32_t k = 0; k < shadow; ++k) {
    const int32_t y = (int32_t)ceilf(l->bar_h) + k;
    if (y >= canvas->height) break;
    uint16_t* row = row_at(canvas, y);
    const uint32_t amount = (uint32_t)(110 * (shadow - k) / shadow);
    for (int32_t x = 0; x < canvas->width; ++x) row[x] = darken(row[x], amount);
  }
}

/* ---- Static parts -------------------------------------------------------- */

static void field_clip(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_layout_t* l) {
  canvas->clip_y0 = (int32_t)ceilf(l->bar_h);
  canvas->clip_y1 = canvas->height;
}

static void full_clip(h2_garden_guard_canvas_t* canvas) {
  canvas->clip_y0 = 0;
  canvas->clip_y1 = 0;
}

static void depth_mode(h2_garden_guard_canvas_t* canvas, uint8_t mode,
                       uint32_t lane) {
  canvas->depth_mode = mode;
  canvas->depth_layer = (uint8_t)(lane + 1u);
}

static uint32_t cooldown_step(const h2_garden_guard_game_t* game,
                              unsigned card) {
  const uint32_t total =
      h2_garden_guard_card_cooldown((h2_garden_guard_card_t)card);
  const uint32_t left = game->card_cooldown_ms[card];
  if (total == 0u || left == 0u) return 0u;
  return (uint32_t)(((uint64_t)left * COOLDOWN_STEPS + total - 1u) / total);
}

static uint32_t progress_step(const h2_garden_guard_game_t* game) {
  const uint32_t total = h2_garden_guard_pests_total();
  const uint32_t left = h2_garden_guard_pests_remaining(game);
  if (total == 0u || left >= total) return 0u;
  return (total - left) * PROGRESS_STEPS / total;
}

static bool prompt_visible(const h2_garden_guard_game_t* game) {
  return (game->phase == H2_GARDEN_GUARD_PHASE_WON ||
          game->phase == H2_GARDEN_GUARD_PHASE_LOST) &&
         game->time_ms - game->phase_started_ms >=
             H2_GARDEN_GUARD_RESTART_DELAY_MS;
}

void h2_garden_guard_static_key(const h2_garden_guard_game_t* game,
                                h2_garden_guard_part_t part,
                                h2_garden_guard_static_key_t* out) {
  memset(out, 0, sizeof(*out));
  out->phase = (uint32_t)game->phase;
  out->values[14] = game->language;
  out->values[15] = game->language_unsaved;
  if (part < H2_GARDEN_GUARD_PART_FIELD) {
    out->values[12] = game->menu;
    if (part == H2_GARDEN_GUARD_PART_SUN) {
      out->values[0] = game->sun;
    } else if (part == H2_GARDEN_GUARD_PART_STATUS) {
      out->values[1] = (uint32_t)(game->selected + 1);
      out->values[8] = game->wave;
      out->values[9] = progress_step(game);
      out->values[11] = game->sighting_ms != 0u ? 1u + game->sighting_kind : 0u;
    } else {
      const unsigned card = (unsigned)part - H2_GARDEN_GUARD_PART_CARD_FIRST;
      const uint32_t cost =
          h2_garden_guard_card_cost((h2_garden_guard_card_t)card);
      out->values[0] = game->sun >= cost;
      out->values[1] = game->selected == (int32_t)card;
      out->values[2] = cooldown_step(game, card);
      out->values[3] = game->card_denied_ms[card] != 0u;
      out->values[4] =
          game->profile.owned & h2_garden_guard_card_unlock((int32_t)card);
    }
    return;
  }
  uint32_t carts = 0;
  for (unsigned lane = 0; lane < LANES; ++lane)
    if (game->carts[lane].state == H2_GARDEN_GUARD_CART_READY)
      carts |= 1u << lane;
  out->values[1] = carts;
  out->values[7] = game->menu;
  if (game->phase == H2_GARDEN_GUARD_PHASE_PLAYING) return;
  out->values[0] = game->plant_revision;
  out->values[2] = game->score;
  out->values[3] = game->best;
  out->values[4] = game->new_best ? 1u : 0u;
  out->values[5] = prompt_visible(game) ? 1u : 0u;
  out->values[6] = game->journal_card;
  out->values[8] = game->profile.total_score;
  out->values[9] = game->profile.coins;
  out->values[10] = game->profile.seen;
  out->values[11] = game->profile.owned;
  out->values[12] = game->save_state;
  out->values[13] = game->earned_coins;
}

static void draw_card(h2_garden_guard_canvas_t* canvas,
                      const h2_garden_guard_game_t* game, unsigned card) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float x0 = l->card_x[card], y0 = l->card_y;
  const float x1 = x0 + l->card_w, y1 = y0 + l->card_h;
  const float radius = l->card_w * 0.14f;
  const uint32_t cost = h2_garden_guard_card_cost((h2_garden_guard_card_t)card);
  const uint8_t unlock = h2_garden_guard_card_unlock((int32_t)card);
  const bool locked = (game->profile.owned & unlock) != unlock;
  const bool affordable = game->sun >= cost && !locked;
  if (game->selected == (int32_t)card) {
    const float grow = fmaxf(2.0f, 3.0f * l->unit);
    rounded_rect(canvas, x0 - grow, y0 - grow, x1 + grow, y1 + grow,
                 radius + grow, RGB(255, 230, 70), 256u);
  }
  rounded_rect(canvas, x0, y0, x1, y1, radius,
               affordable ? RGB(238, 222, 180) : RGB(150, 144, 132), 256u);
  const float inset = fmaxf(2.0f, 2.0f * l->unit);
  const float well_y1 = y0 + l->card_h * 0.7f;
  rounded_rect(canvas, x0 + inset, y0 + inset, x1 - inset, well_y1,
               radius * 0.7f,
               affordable ? RGB(127, 170, 149) : RGB(101, 128, 118), 256u);
  /* A strip of lawn at the bottom of the well for the figure to stand on. */
  const float well_h = well_y1 - y0 - inset;
  rounded_rect(canvas, x0 + inset, well_y1 - well_h * 0.24f, x1 - inset,
               well_y1, radius * 0.5f,
               affordable ? RGB(112, 151, 92) : RGB(98, 117, 101), 256u);
  const float cx = (x0 + x1) * 0.5f;
  if (card == H2_GARDEN_GUARD_CARD_SHOVEL) {
    draw_shovel(canvas, cx, (y0 + inset + well_y1) * 0.5f,
                fminf(l->card_w - 2.0f * inset, well_h));
  } else {
    const uint8_t kind =
        card == H2_GARDEN_GUARD_CARD_FROST     ? H2_GARDEN_GUARD_PLANT_FROST
        : card == H2_GARDEN_GUARD_CARD_BRAMBLE ? H2_GARDEN_GUARD_PLANT_BRAMBLE
                                               : (uint8_t)(card + 1u);
    const float portrait_limit = l->card_w - 2.0f * inset;
    const float scale = fminf(well_h * 0.92f, portrait_limit * 0.84f);
    float gx = cx, ground_y = well_y1 - well_h * 0.12f;
    // Center the existing head art in its icon well, omitting the feet and
    // stem.
    if (kind == H2_GARDEN_GUARD_PLANT_SUNFLOWER) {
      ground_y = y0 + inset + well_h * 0.49f + scale * 0.725f;
    } else if (kind == H2_GARDEN_GUARD_PLANT_FROST ||
               kind == H2_GARDEN_GUARD_PLANT_SHOOTER) {
      ground_y = y0 + inset + well_h * 0.57f + scale * 0.54f;
      gx -= 0.035f * scale;
    } else {
      ground_y = y0 + inset + well_h * 0.57f + scale * 0.295f;
    }
    draw_plant_kind(canvas, kind, gx, ground_y, scale, 0u, k_neutral_plant,
                    true);
  }
  if (cost > 0u) {
    char value[12];
    if (locked)
      (void)snprintf(value, sizeof(value), "%s",
                     l->card_w < 35.0f ? L("L", "锁") : L("LOCK", "未解锁"));
    else
      (void)snprintf(value, sizeof(value), "%u", (unsigned)cost);
    const float text_h = y1 - well_y1;
    int32_t scale = (int32_t)(text_h * 0.62f / 7.0f + 0.5f);
    if (scale < 1) scale = 1;
    scale = fit_scale(value, l->card_w - 2.0f * inset, scale);
    text_centered(canvas, value, cx,
                  well_y1 + (text_h - 7.0f * (float)scale) * 0.5f, scale,
                  affordable ? RGB(70, 44, 20) : RGB(70, 66, 60), 256u);
  }
  const uint32_t step = cooldown_step(game, card);
  if (step > 0u) {
    /* The shade drains from the top as the card recharges. */
    const float bottom = y0 + l->card_h * (float)step / (float)COOLDOWN_STEPS;
    rounded_rect(canvas, x0, y0, x1, bottom, fminf(radius, bottom - y0),
                 RGB(16, 12, 8), 140u);
  }
  if (game->card_denied_ms[card] != 0u && !locked)
    rounded_rect(canvas, x0, y0, x1, y1, radius, RGB(230, 40, 32), 120u);
}

typedef struct zombie_words {
  const char* name;
  const char* trait;
  const char* counter;
} zombie_words_t;

static const zombie_words_t k_zombie_cards[2][H2_GARDEN_GUARD_PEST_KIND_COUNT] =
    {
        {{"GARDENER", "DRAGS ONE FOOT", "KEEP SHOOTERS BEHIND WALLS"},
         {"SHELLBACK", "PLATE TAKES THE FIRST HITS",
          "STACK SHOOTERS IN ITS LANE"},
         {"RUNNER", "FAST BUT FRAGILE", "BLOCK IT OR USE FROST"}},
        {{"园丁僵尸", "拖着脚慢慢走", "把射手种在墙后"},
         {"铁甲僵尸", "护甲先吸收伤害", "同一路多种射手"},
         {"疾跑僵尸", "跑得快，血量低", "种墙阻挡或用冰铃减速"}},
};

static void menu_button(h2_garden_guard_canvas_t* canvas,
                        h2_garden_guard_rect_t r, const char* label,
                        uint16_t fill, uint16_t ink);

static void draw_sun_counter(h2_garden_guard_canvas_t* canvas,
                             const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  rounded_rect(canvas, l->sun_x, l->card_y, l->sun_x + l->sun_w,
               l->card_y + l->card_h, l->card_w * 0.14f, RGB(20, 41, 44), 170u);
  float sx, sy;
  h2_garden_guard_sun_counter_point(l, &sx, &sy);
  draw_sun(canvas, sx, sy, l->card_h * 0.2f, 0.2f, 256u);
  char value[32];
  (void)snprintf(value, sizeof(value), "%u", (unsigned)game->sun);
  int32_t scale = (int32_t)(l->card_h * 0.26f / 7.0f + 0.5f);
  if (scale < 1) scale = 1;
  scale = fit_scale(value, l->sun_w * 0.9f, scale);
  text_centered(canvas, value, l->sun_x + l->sun_w * 0.5f,
                l->card_y + l->card_h * 0.64f, scale, RGB(255, 240, 200), 256u);
}

static void draw_toolbar_status(h2_garden_guard_canvas_t* canvas,
                                const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float u = l->unit;
  char value[32];
  /* Wave label and progress toward the end of the last wave. */
  const float pad = l->card_y;
  const float x0 =
      l->card_x[H2_GARDEN_GUARD_CARD_COUNT - 1] + l->card_w + 3.0f * pad + 2.0f;
  const h2_garden_guard_rect_t pause =
      h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_PAUSE);
  if (game->phase == H2_GARDEN_GUARD_PHASE_PLAYING)
    menu_button(canvas, pause, (pause.w < 45.0f ? "II" : L("MENU", "菜单")),
                RGB(58, 100, 82), RGB(252, 232, 177));
  const float x1 = game->phase == H2_GARDEN_GUARD_PHASE_PLAYING
                       ? pause.x - pad
                       : (float)l->width - 2.0f * pad - 2.0f;
  if (x1 - x0 < 24.0f * u) return;
  if (game->wave == 0u)
    (void)snprintf(value, sizeof(value), "%s", L("READY", "准备中"));
  else
    (void)snprintf(value, sizeof(value), L("WAVE %u/%u", "第%u/%u波"),
                   (unsigned)game->wave, (unsigned)H2_GARDEN_GUARD_WAVES);
  if ((float)h2_garden_guard_text_width(value, 1) > x1 - x0) {
    if (game->wave == 0u)
      (void)snprintf(value, sizeof(value), "%s", L("SET", "准备"));
    else
      (void)snprintf(value, sizeof(value), L("W%u/%u", "%u/%u波"),
                     (unsigned)game->wave, (unsigned)H2_GARDEN_GUARD_WAVES);
  }
  int32_t label = (int32_t)(l->card_h * 0.22f / 7.0f + 0.5f);
  if (label < 1) label = 1;
  label = fit_scale(value, x1 - x0, label);
  text_centered(canvas, value, (x0 + x1) * 0.5f, l->card_y + l->card_h * 0.12f,
                label, RGB(250, 236, 200), 256u);
  const float bar_y0 = l->card_y + l->card_h * 0.56f;
  const float bar_y1 = l->card_y + l->card_h * 0.8f;
  const float radius = (bar_y1 - bar_y0) * 0.5f;
  rounded_rect(canvas, x0, bar_y0, x1, bar_y1, radius, RGB(18, 38, 39), 220u);
  const uint32_t step = progress_step(game);
  const char* selection = NULL;
  if (game->selected >= 0 && game->selected < H2_GARDEN_GUARD_CARD_COUNT) {
    static const char* const names[2][H2_GARDEN_GUARD_CARD_COUNT] = {
        {"SUNFLOWER", "SHOOTER", "PUMPKIN", "SHOVEL", "FROSTBELL", "BRAMBLE"},
        {"向日葵", "豌豆射手", "南瓜墙", "铲子", "冰铃", "荆棘球"},
    };
    const char* name =
        names[game->language == H2_GARDEN_GUARD_LANG_ZH][game->selected];
    if ((float)h2_garden_guard_text_width(name, 1) <= x1 - x0) selection = name;
  }
  if (game->sighting_ms != 0u) {
    const char* sighting = x1 - x0 >= 96.0f
                               ? L("NEW JOURNAL CARD", "图鉴已收录")
                               : L("NEW", "新卡");
    text_centered(
        canvas, sighting, (x0 + x1) * 0.5f,
        bar_y0 + (bar_y1 - bar_y0 - (float)label_height(sighting, 1)) * 0.5f,
        fit_scale(sighting, x1 - x0, 1), RGB(255, 218, 119), 256u);
  } else if (selection != NULL) {
    text_centered(
        canvas, selection, (x0 + x1) * 0.5f,
        bar_y0 + (bar_y1 - bar_y0 - (float)label_height(selection, 1)) * 0.5f,
        1, RGB(224, 237, 171), 256u);
  } else if (step > 0u) {
    const float inset = fmaxf(1.0f, 2.0f * u);
    const float fill =
        (x1 - x0 - 2.0f * inset) * (float)step / (float)PROGRESS_STEPS;
    rounded_rect(canvas, x0 + inset, bar_y0 + inset, x0 + inset + fill,
                 bar_y1 - inset, fmaxf(radius - inset, 0.0f), RGB(132, 206, 72),
                 256u);
  }
}

#include "h2_garden_guard_title.inc"

static uint8_t title_symbol(uint32_t* offset) {
  int32_t node = (int32_t)(sizeof(k_title_tree) / sizeof(k_title_tree[0])) - 1;
  uint32_t pos = *offset;
  while (node >= 0) {
    const uint32_t branch =
        ((uint32_t)k_title_art[pos >> 3] >> (7u - (pos & 7u))) & 1u;
    node = k_title_tree[node][branch];
    ++pos;
  }
  *offset = pos;
  return (uint8_t)(-node - 1);
}

/* Expand a single source row when the scale needs it. The illustration uses
 * flash plus a scanline on the stack. No extra framebuffer or codec is needed.
 */
static void draw_title_art(h2_garden_guard_canvas_t* canvas, float x, float y,
                           float scale) {
  region_t bounds;
  if (!region_begin(canvas, x, y, x + TITLE_ART_WIDTH * scale,
                    y + TITLE_ART_HEIGHT * scale, &bounds))
    return;
  const float inverse = 1.0f / scale;
  uint8_t line[TITLE_ART_WIDTH];
  int32_t decoded = -1;
  for (int32_t py = bounds.y0; py < bounds.y1; ++py) {
    const int32_t sy = (int32_t)clampf(((float)py + 0.5f - y) * inverse, 0.0f,
                                       (float)(TITLE_ART_HEIGHT - 1));
    if (decoded != sy) {
      uint32_t offset = k_title_bit_offsets[sy];
      int32_t sx = 0;
      while (sx < TITLE_ART_WIDTH) {
        const uint8_t token = title_symbol(&offset);
        if ((token & 128u) == 0u) {
          line[sx++] = token;
        } else {
          const size_t count = (token & 127u) + 3u;
          memset(&line[sx], title_symbol(&offset), count);
          sx += (int32_t)count;
        }
      }
      decoded = sy;
    }
    uint16_t* row = row_at(canvas, py);
    for (int32_t px = bounds.x0; px < bounds.x1; ++px) {
      const int32_t sx = (int32_t)clampf(((float)px + 0.5f - x) * inverse, 0.0f,
                                         (float)(TITLE_ART_WIDTH - 1));
      row[px] = k_title_palette[line[sx]];
    }
  }
}

static void menu_button(h2_garden_guard_canvas_t* canvas,
                        h2_garden_guard_rect_t r, const char* label,
                        uint16_t fill, uint16_t ink) {
  rounded_rect(canvas, r.x, r.y, r.x + r.w, r.y + r.h, r.h * 0.23f, fill, 256u);
  const int32_t scale = fit_scale(label, r.w * 0.93f, r.h >= 25.0f ? 2 : 1);
#ifdef __EMSCRIPTEN__
  if (canvas->external_menu_labels) {
    collect_menu_label(canvas, label, r.x + r.w * 0.5f,
                       r.y + (r.h - (float)label_height(label, scale)) * 0.5f,
                       scale, ink, 256u, true);
    return;
  }
#endif
  const int32_t width = h2_garden_guard_text_width(label, scale);
  text_layer(canvas, label, (int32_t)(r.x + (r.w - (float)width) * 0.5f),
             (int32_t)(r.y + (r.h - (float)label_height(label, scale)) * 0.5f),
             scale, ink, 256u);
}

static void menu_tabs(h2_garden_guard_canvas_t* canvas,
                      const h2_garden_guard_game_t* game, bool result) {
  const h2_garden_guard_layout_t* l = &game->layout;
  menu_button(canvas, h2_garden_guard_menu_tab(l, result, 0u),
              L("JOURNAL", "图鉴"), RGB(50, 93, 78), RGB(253, 235, 182));
  menu_button(canvas, h2_garden_guard_menu_tab(l, result, 1u),
              L("SHOP", "商店"), RGB(50, 93, 78), RGB(253, 235, 182));
}

static void journal_card_face(h2_garden_guard_canvas_t* canvas,
                              const h2_garden_guard_game_t* game,
                              h2_garden_guard_rect_t r, unsigned kind,
                              bool detail) {
  const bool known = (game->profile.seen & (1u << kind)) != 0u;
  const float k = r.w / 128.0f;
  const float cx = r.x + r.w * 0.5f;
  const float x1 = r.x + r.w, y1 = r.y + r.h;
  const float edge = fmaxf(1.0f, 2.0f * k);
  const float picture_bottom = r.y + r.h * (detail ? 0.55f : 0.72f);
  const float picture_top = r.y + 9.0f * k;
  static const uint16_t backgrounds[] = {RGB(160, 168, 113), RGB(125, 160, 150),
                                         RGB(178, 150, 100)};
  const uint16_t paper = known ? RGB(237, 219, 178) : RGB(63, 88, 71);
  const uint16_t ink = known ? RGB(35, 59, 47) : RGB(239, 225, 179);
  const uint16_t accent = known ? RGB(144, 107, 57) : RGB(145, 161, 124);
  rounded_rect(canvas, r.x, r.y + 3.0f * k, x1, y1 + 3.0f * k, 8.0f * k,
               RGB(10, 25, 19), 204u);
  rounded_rect(canvas, r.x, r.y, x1, y1, 8.0f * k, accent, 256u);
  rounded_rect(canvas, r.x + edge, r.y + edge, x1 - edge, y1 - edge, 7.0f * k,
               paper, 256u);
  rounded_rect(canvas, r.x + 9.0f * k, picture_top, x1 - 9.0f * k,
               picture_bottom, 6.0f * k,
               known ? backgrounds[kind] : RGB(44, 70, 57), 256u);
  if (known) {
    // The bust uses the authored portrait and is rerasterized at the displayed
    // size.
    const float picture_height = picture_bottom - picture_top;
    h2_garden_guard_render_zombie_portrait(
        canvas, (h2_garden_guard_pest_kind_t)kind, cx,
        picture_bottom - 4.0f * k, fminf(r.w * 0.76f, picture_height * 0.82f));
  } else {
    const float cy = (picture_top + picture_bottom) * 0.5f;
    odisc(canvas, cx, cy, 21.5f * k, edge, RGB(51, 80, 63), RGB(112, 135, 103));
    text_centered(canvas, "?", cx, cy - 10.0f * k, text_scale(k, 3.0f),
                  RGB(214, 212, 161), 256u);
  }
  const zombie_words_t* words =
      &k_zombie_cards[game->language == H2_GARDEN_GUARD_LANG_ZH][kind];
  const char* name = known ? words->name : L("UNKNOWN", "未收录");
  if (r.w < 63.0f && known) {
    static const char* const short_names[2][3] = {
        {"GARDEN", "PLATED", "RUNNER"}, {"园丁", "铁甲", "疾跑"}};
    name = short_names[game->language == H2_GARDEN_GUARD_LANG_ZH][kind];
  }
  const int32_t name_scale =
      fit_scale(name, r.w - 16.0f * k, text_scale(k, detail ? 2.2f : 2.0f));
  const float name_y = picture_bottom + 8.0f * k;
#ifdef __EMSCRIPTEN__
  if (canvas->external_menu_labels)
    collect_menu_label(canvas, name, cx, name_y, name_scale, ink, 256u, true);
  else
#endif
    text_layer(
        canvas, name,
        (int32_t)(cx -
                  (float)h2_garden_guard_text_width(name, name_scale) * 0.5f),
        (int32_t)name_y, name_scale, ink, 256u);
  if (!detail) {
    const char* action = known          ? L("VIEW", "查看")
                         : r.w >= 80.0f ? L("MEET IN PLAY", "对局中遇见")
                                        : L("IN PLAY", "去对局");
    text_centered(canvas, action, cx, y1 - 21.0f * k, text_scale(k, 1.0f),
                  known ? RGB(68, 96, 63) : ink, 256u);
    return;
  }
  capsule(canvas, r.x + 12.0f * k, r.y + 132.0f * k, x1 - 12.0f * k,
          r.y + 132.0f * k, fmaxf(0.5f, 0.6f * k), accent, 168u);
  const char* trait = words->trait;
  const char* counter = words->counter;
  if (r.w < 158.0f) {
    static const char* const short_traits[2][3] = {
        {"DRAGS ONE FOOT", "ABSORBS HITS", "FAST, FRAGILE"},
        {"拖着脚走", "护甲挡伤", "跑快血少"}};
    static const char* const short_counters[2][3] = {
        {"USE WALLS", "STACK SHOOTERS", "BLOCK OR FROST"},
        {"用墙阻挡", "多种射手", "用墙或冰铃"}};
    const bool zh = game->language == H2_GARDEN_GUARD_LANG_ZH;
    trait = short_traits[zh][kind];
    counter = short_counters[zh][kind];
  }
  const float text_x = r.x + 12.0f * k;
  const char* labels[] = {L("TRAIT", "特征"), L("COUNTER", "应对")};
  if (r.w < 85.0f && game->language == H2_GARDEN_GUARD_LANG_EN) {
    static const char* const compact_traits[] = {"DRAG STEP", "ARMORED",
                                                 "FAST"};
    static const char* const compact_counters[] = {"USE WALLS", "MORE PEAS",
                                                   "USE FROST"};
    trait = compact_traits[kind];
    counter = compact_counters[kind];
  }
  const char* info[] = {trait, counter};
  for (unsigned i = 0; i < 2u; ++i) {
    const float line_y = r.y + (137.0f + 25.0f * (float)i) * k;
    text_layer(canvas, labels[i], (int32_t)text_x, (int32_t)line_y, 1,
               RGB(111, 80, 41), 256u);
    text_layer(canvas, info[i], (int32_t)text_x,
               (int32_t)(line_y + fmaxf(9.0f, 12.0f * k)), 1, ink, 256u);
  }
}

static void draw_journal(h2_garden_guard_canvas_t* canvas,
                         const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const h2_garden_guard_rect_t back = h2_garden_guard_menu_back(l);
  const float s = back.w / 80.0f;
  const float cx = (float)l->width * 0.5f;
  const float top = back.y - 16.0f * s;
  const uint16_t cream = RGB(252, 228, 172);
  fill_rect(canvas, 0.0f, 0.0f, (float)l->width, (float)l->height,
            RGB(22, 44, 37), 256u);
  capsule(canvas, cx - 205.0f * s, top + 69.0f * s, cx + 205.0f * s,
          top + 69.0f * s, fmaxf(0.5f, s), RGB(141, 130, 82), 235u);
  menu_button(canvas, back, L("BACK", "返回"), RGB(47, 88, 74), cream);
  const char* heading =
      s < 0.52f ? L("JOURNAL", "图鉴") : L("FIELD JOURNAL", "僵尸图鉴");
  text_centered(canvas, heading, cx, top + 22.0f * s,
                fit_scale(heading, 248.0f * s, text_scale(s, 3.0f)), cream,
                256u);
  unsigned known = 0;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_PEST_KIND_COUNT; ++i) {
    known += (game->profile.seen >> i) & 1u;
    if (game->journal_card != i + 1u)
      journal_card_face(canvas, game, h2_garden_guard_journal_card_rect(l, i),
                        i, false);
  }
  char count[32];
  (void)snprintf(count, sizeof(count), L("%u/%u COLLECTED", "已收录%u/%u"),
                 known, (unsigned)H2_GARDEN_GUARD_PEST_KIND_COUNT);
  text_centered(canvas, count, cx, top + 50.0f * s,
                fit_scale(count, 250.0f * s, text_scale(s, 2.0f)),
                RGB(240, 197, 103), 256u);
}

static void draw_journal_focus(h2_garden_guard_canvas_t* canvas,
                               const h2_garden_guard_game_t* game) {
  const unsigned kind = game->journal_card - 1u;
  const h2_garden_guard_layout_t* l = &game->layout;
  const float progress =
      (float)game->journal_progress_ms / H2_GARDEN_GUARD_JOURNAL_MS;
  // A single, reversible cubic ease keeps the opening and closing paths
  // aligned.
  const float eased = progress * progress * (3.0f - 2.0f * progress);
  const h2_garden_guard_rect_t from =
      h2_garden_guard_journal_card_rect(l, kind);
  const h2_garden_guard_rect_t to = h2_garden_guard_journal_detail_rect(l);
  h2_garden_guard_rect_t paint = {
      from.x + (to.x - from.x) * eased, from.y + (to.y - from.y) * eased,
      from.w + (to.w - from.w) * eased, from.h + (to.h - from.h) * eased};
#ifdef __EMSCRIPTEN__
  if (canvas->menu_labels != NULL) {
    h2_garden_guard_menu_labels_t* menu = canvas->menu_labels;
    menu->foreground = menu->count;
    menu->card[0] = paint.x;
    menu->card[1] = paint.y;
    menu->card[2] = paint.w;
    menu->card[3] = paint.h;
    menu->shade = eased * (180.0f / 256.0f);
  }
#endif
  if (eased > 0.0f)
    fill_rect(canvas, 0.0f, 0.0f, (float)l->width, (float)l->height,
              RGB(13, 26, 21), (uint32_t)(eased * 180.0f));
  const bool settled = !h2_garden_guard_journal_animating(game);
  journal_card_face(canvas, game, paint, kind, settled);
  if (settled) {
    const h2_garden_guard_rect_t close = h2_garden_guard_journal_close_rect(l);
    menu_button(canvas, close, close.w < 23.0f ? "X" : L("X", "关闭"),
                RGB(39, 74, 62), RGB(251, 231, 185));
  }
}

static void draw_shop(h2_garden_guard_canvas_t* canvas,
                      const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const h2_garden_guard_rect_t back = h2_garden_guard_menu_back(l);
  const float s = back.w / 80.0f;
  const float cx = (float)l->width * 0.5f;
  const uint16_t paper = RGB(235, 216, 173);
  const uint16_t ink = RGB(35, 61, 51);
  const uint16_t muted = RGB(87, 111, 88);
  const uint16_t light = RGB(244, 226, 176);
  fill_rect(canvas, 0.0f, 0.0f, (float)l->width, (float)l->height,
            RGB(19, 39, 35), 256u);
  menu_button(canvas, back, L("BACK", "返回"), RGB(48, 88, 76), light);
  const char* heading = L("PLANT SHOP", "植物商店");
  text_centered(canvas, heading, cx, back.y + 6.0f * s,
                fit_scale(heading, 252.0f * s, text_scale(s, 3.0f)), light,
                256u);
  char label[40];
  (void)snprintf(label, sizeof(label), L("%u COINS", "%u金币"),
                 (unsigned)game->profile.coins);
  text_centered(canvas, label, cx, back.y + 39.0f * s,
                fit_scale(label, 260.0f * s, text_scale(s, 2.0f)),
                RGB(240, 197, 103), 256u);
  for (unsigned i = 0; i < 2u; ++i) {
    const h2_garden_guard_rect_t r = h2_garden_guard_menu_row(l, i);
    rounded_rect(canvas, r.x, r.y, r.x + r.w, r.y + r.h, 8.0f * s, paper, 256u);
    rounded_rect(canvas, r.x + 5.0f * s, r.y + 5.0f * s, r.x + 79.0f * s,
                 r.y + r.h - 5.0f * s, 5.0f * s, RGB(160, 177, 127), 256u);
    const uint8_t kind =
        i == 0u ? H2_GARDEN_GUARD_PLANT_FROST : H2_GARDEN_GUARD_PLANT_BRAMBLE;
    draw_plant_kind(canvas, kind, r.x + 43.0f * s, r.y + 52.0f * s, 42.0f * s,
                    0u, k_neutral_plant, false);
    const char* name =
        i == 0u ? L("FROSTBELL", "冰铃") : L("BRAMBLE", "荆棘球");
    const char* trait = i == 0u
                            ? L("SLOWS WITH ICY SHOTS", "冰弹减缓僵尸")
                            : L("THORNS HURT BITING ZOMBIES", "反击啃咬的僵尸");
    const char* hint = i == 0u ? L("125 SUN TO PLANT", "种植需125阳光")
                               : L("75 SUN TO PLANT", "种植需75阳光");
    if (s < 0.52f) {
      trait = i == 0u ? L("SLOWS", "减速") : L("THORN HITS", "反伤");
      hint = i == 0u ? L("125 SUN", "125阳光") : L("75 SUN", "75阳光");
    }
    const float x = r.x + 92.0f * s;
    const float available = 226.0f * s;
    const float name_y = r.y + (s < 0.42f ? 0.0f : 10.0f * s);
    const float trait_y = r.y + (s < 0.42f ? 7.0f : 31.0f * s);
    const float hint_y = r.y + (s < 0.42f ? 14.0f : 45.0f * s);
    text_layer(canvas, name, (int32_t)x, (int32_t)name_y,
               fit_scale(name, available, text_scale(s, 2.0f)), ink, 256u);
    text_layer(canvas, trait, (int32_t)x, (int32_t)trait_y,
               fit_scale(trait, available, 1), ink, 256u);
    if (hint != NULL)
      text_layer(canvas, hint, (int32_t)x, (int32_t)hint_y,
                 fit_scale(hint, available, 1), muted, 256u);
    const uint32_t price = h2_garden_guard_offer_price(i);
    const bool owned =
        (game->profile.owned & (i == 0u ? H2_GARDEN_GUARD_FROST_BIT
                                        : H2_GARDEN_GUARD_BRAMBLE_BIT)) != 0u;
    const bool enough = game->profile.coins >= price;
    (void)snprintf(label, sizeof(label),
                   owned    ? L("OWNED", "已拥有")
                   : enough ? L("BUY %u", "买入%u")
                            : L("NEED %u", "需%u"),
                   (unsigned)price);
    const h2_garden_guard_rect_t buy = {r.x + r.w * 0.74f + 7.0f * s,
                                        r.y + 15.0f * s,
                                        r.w * 0.26f - 14.0f * s, 32.0f * s};
    if (!owned && (float)h2_garden_guard_text_width(label, 1) > buy.w)
      (void)snprintf(label, sizeof(label),
                     enough ? L("%u C", "%u币") : L("NO %u", "需%u"),
                     (unsigned)price);
    menu_button(canvas, buy, label,
                owned    ? RGB(126, 162, 120)
                : enough ? RGB(252, 199, 91)
                         : RGB(182, 168, 138),
                ink);
  }
  const h2_garden_guard_rect_t row = h2_garden_guard_menu_row(l, 2u);
  (void)snprintf(label, sizeof(label), L("TOTAL SCORE %u", "累计%u分"),
                 (unsigned)game->profile.total_score);
  text_centered(canvas, label, cx, row.y + 9.0f * s,
                fit_scale(label, 420.0f * s, text_scale(s, 2.0f)), light, 256u);
  const char* hint = s < 0.52f ? L("PLANT KILLS EARN COINS", "击倒僵尸可赚金币")
                               : L("PLANT DEFEATS EARN COINS AT THE END",
                                   "植物击倒僵尸可获结算金币");
  text_centered(canvas, hint, cx, row.y + 38.0f * s,
                fit_scale(hint, 435.0f * s, 1), light, 256u);
  if (game->save_state != H2_GARDEN_GUARD_SAVE_READY ||
      game->language_unsaved) {
    const char* status = (game->save_state == H2_GARDEN_GUARD_SAVE_SESSION ||
                          game->language_unsaved)
                             ? L("SESSION ONLY", "仅本次有效")
                             : L("SAVE PENDING", "正在重试保存");
    text_centered(canvas, status, cx, back.y + 289.0f * s,
                  fit_scale(status, 420.0f * s, 1), RGB(244, 184, 114), 256u);
  }
}

static void cover_type(h2_garden_guard_canvas_t* canvas, const uint8_t* glyphs,
                       unsigned side, float cx, float cy, float scale,
                       uint16_t color) {
  const float gap = side >= 24u ? 2.0f : 1.0f;
  const float width = (4.0f * (float)side + 3.0f * gap) * scale;
  const float x = cx - width * 0.5f, y = cy - (float)side * scale * 0.5f;
  region_t bounds;
  if (!region_begin(canvas, x, y, x + width, y + (float)side * scale, &bounds))
    return;
  const unsigned pitch = side * side / 2u;
  const float advance = (float)side + gap;
  for (int32_t py = bounds.y0; py < bounds.y1; ++py) {
    const int32_t sy = (int32_t)floorf(((float)py + 0.5f - y) / scale);
    if (sy < 0 || (unsigned)sy >= side) continue;
    uint16_t* row = row_at(canvas, py);
    for (int32_t px = bounds.x0; px < bounds.x1; ++px) {
      const float u = ((float)px + 0.5f - x) / scale;
      const unsigned glyph = (unsigned)fmaxf(0.0f, floorf(u / advance));
      const int32_t sx = (int32_t)floorf(u - (float)glyph * advance);
      if (glyph >= 4u || sx < 0 || (unsigned)sx >= side) continue;
      const unsigned i = (unsigned)sy * side + (unsigned)sx;
      const uint8_t packed = glyphs[glyph * pitch + i / 2u];
      const uint32_t alpha = (packed >> ((i & 1u) * 4u)) & 15u;
      if (alpha != 0u) plot(canvas, row, NULL, px, color, alpha * 256u / 15u);
    }
  }
}

static void cover_panel(h2_garden_guard_canvas_t* canvas,
                        h2_garden_guard_rect_t r, uint16_t color) {
  const float s = r.w / 133.0f, x1 = r.x + r.w, y1 = r.y + r.h;
  const float bevel = fmaxf(1.0f, 3.0f * s);
  rounded_rect(canvas, r.x, r.y + 4.0f * s, x1, y1 + 4.0f * s, 4.0f * s,
               RGB(11, 24, 18), 235u);
  rounded_rect(canvas, r.x, r.y, x1, y1, 5.0f * s, RGB(77, 43, 29), 256u);
  rounded_rect(canvas, r.x + bevel, r.y + bevel, x1 - bevel, y1 - bevel,
               3.0f * s, color, 256u);
  fill_rect(canvas, r.x + bevel, r.y + bevel, x1 - bevel, r.y + 5.0f * s,
            RGB(238, 203, 122), 170u);
  for (int side = 0; side < 2; ++side) {
    const float x = side == 0 ? r.x + 8.0f * s : x1 - 8.0f * s;
    disc(canvas, x, r.y + 8.0f * s, 1.45f * s, RGB(239, 211, 133), 256u);
  }
}

static void cover_gate(h2_garden_guard_canvas_t* canvas, float x, float y,
                       float s) {
  ellipse(canvas, x, y + 20.0f * s, 48.0f * s, 7.0f * s, RGB(52, 58, 30), 133u);
  ellipse(canvas, x, y - 3.0f * s, 30.0f * s, 28.0f * s, RGB(251, 221, 130),
          219u);
  disc(canvas, x + 9.0f * s, y - 10.0f * s, 8.0f * s, RGB(255, 241, 179), 256u);
  trapezoid(canvas, y + 3.0f * s, x - 24.0f * s, x + 24.0f * s, y + 16.0f * s,
            x - 24.0f * s, x + 24.0f * s, RGB(147, 159, 81), 256u);
  trapezoid(canvas, y + 8.0f * s, x - 8.0f * s, x + 9.0f * s, y + 24.0f * s,
            x - 24.0f * s, x + 25.0f * s, RGB(226, 193, 113), 256u);
  ocapsule(canvas, x - 25.0f * s, y + 20.0f * s, x - 25.0f * s, y - 14.0f * s,
           3.4f * s, 1.0f * s, RGB(205, 142, 78), RGB(74, 64, 38));
  ocapsule(canvas, x + 25.0f * s, y + 20.0f * s, x + 25.0f * s, y - 14.0f * s,
           3.4f * s, 1.0f * s, RGB(205, 142, 78), RGB(74, 64, 38));
  for (int side = -1; side <= 1; side += 2) {
    const float post = x + (float)side * 26.0f * s;
    capsule(canvas, post, y - 14.0f * s, x + (float)side * 14.0f * s,
            y - 23.0f * s, 3.5f * s, RGB(87, 66, 36), 256u);
    capsule(canvas, post, y - 15.0f * s, x + (float)side * 14.0f * s,
            y - 24.0f * s, 1.8f * s, RGB(225, 166, 88), 256u);
  }
  capsule(canvas, x - 14.0f * s, y - 23.0f * s, x + 14.0f * s, y - 23.0f * s,
          3.1f * s, RGB(188, 132, 69), 256u);
  for (int side = -1; side <= 1; side += 2) {
    const float edge = x + (float)side * 26.0f * s;
    trapezoid(canvas, y - 7.0f * s, edge - 11.0f * s, edge + 11.0f * s,
              y + 17.0f * s, edge - 10.0f * s, edge + 10.0f * s,
              RGB(150, 95, 54), 256u);
    for (int n = -1; n <= 1; ++n) {
      const float plank = edge + (float)n * 6.0f * s;
      capsule(canvas, plank, y - 6.0f * s, plank, y + 16.0f * s, 1.9f * s,
              RGB(221, 163, 90), 256u);
    }
  }
  for (int i = -1; i <= 1; ++i) {
    const float xx = x + (float)i * 14.0f * s;
    ellipse(canvas, xx + 20.0f * s, y + 13.0f * s, 13.0f * s, 6.0f * s,
            RGB(78, 126, 62), 256u);
    ellipse(canvas, xx - 22.0f * s, y + 15.0f * s, 12.0f * s, 5.0f * s,
            RGB(98, 148, 72), 256u);
  }
}

static void cover_book(h2_garden_guard_canvas_t* canvas, float x, float y,
                       float s) {
  ellipse(canvas, x, y + 19.0f * s, 44.0f * s, 7.0f * s, RGB(22, 46, 36), 160u);
  for (int i = 0; i < 2; ++i) {
    const float side = (float)i * 24.0f * s;
    trapezoid(canvas, y - 19.0f * s, x - 29.0f * s + side, x - 2.0f * s + side,
              y + 10.0f * s, x - 23.0f * s + side, x + 4.0f * s + side,
              RGB(222, 185, 103), 256u);
    trapezoid(canvas, y - 16.0f * s, x - 26.0f * s + side, x - 5.0f * s + side,
              y + 7.0f * s, x - 21.0f * s + side, x + 1.0f * s + side,
              RGB(92, 114, 77), 256u);
  }
  trapezoid(canvas, y - 7.0f * s, x - 29.0f * s, x + 25.0f * s, y + 23.0f * s,
            x - 26.0f * s, x + 28.0f * s, RGB(70, 46, 33), 256u);
  trapezoid(canvas, y - 8.0f * s, x - 31.0f * s, x + 20.0f * s, y + 16.0f * s,
            x - 24.0f * s, x + 28.0f * s, RGB(169, 119, 66), 256u);
  capsule(canvas, x - 24.0f * s, y + 18.0f * s, x + 23.0f * s, y + 18.0f * s,
          1.8f * s, RGB(248, 219, 161), 256u);
  capsule(canvas, x - 26.0f * s, y - 7.0f * s, x - 19.0f * s, y + 14.0f * s,
          1.4f * s, RGB(228, 179, 103), 245u);
  capsule(canvas, x + 15.0f * s, y - 7.0f * s, x + 22.0f * s, y + 14.0f * s,
          1.1f * s, RGB(123, 81, 48), 240u);
  oellipse(canvas, x, y + 4.0f * s, 9.0f * s, 7.0f * s, s, RGB(217, 167, 75),
           RGB(111, 73, 37));
  disc(canvas, x, y + 3.0f * s, 1.7f * s, RGB(96, 60, 31), 256u);
  fill_rect(canvas, x - s, y + 4.0f * s, x + s, y + 7.0f * s, RGB(96, 60, 31),
            256u);
}

static void cover_market(h2_garden_guard_canvas_t* canvas, float x, float y,
                         float s) {
  ellipse(canvas, x, y + 19.0f * s, 46.0f * s, 6.0f * s, RGB(43, 47, 28), 180u);
  fill_rect(canvas, x - 30.0f * s, y - 6.0f * s, x - 27.0f * s, y + 21.0f * s,
            RGB(225, 169, 102), 256u);
  fill_rect(canvas, x + 27.0f * s, y - 6.0f * s, x + 30.0f * s, y + 21.0f * s,
            RGB(225, 169, 102), 256u);
  for (int i = -3; i < 3; ++i) {
    const float u = (float)i * 10.0f * s;
    trapezoid(canvas, y - 20.0f * s, x + 0.8f * u, x + 0.8f * u + 8.0f * s,
              y - 7.0f * s, x + u, x + u + 10.0f * s,
              (i & 1) ? RGB(230, 182, 107) : RGB(168, 93, 67), 256u);
  }
  capsule(canvas, x - 31.0f * s, y + 15.0f * s, x + 30.0f * s, y + 15.0f * s,
          2.4f * s, RGB(117, 73, 46), 256u);
  capsule(canvas, x - 30.0f * s, y + 14.0f * s, x + 29.0f * s, y + 14.0f * s,
          .8f * s, RGB(225, 169, 102), 256u);
  trapezoid(canvas, y + 5.0f * s, x - 18.0f * s, x + 4.0f * s, y + 22.0f * s,
            x - 14.0f * s, x, RGB(176, 98, 65), 256u);
  ellipse(canvas, x - 7.0f * s, y + 5.0f * s, 11.0f * s, 3.2f * s,
          RGB(233, 150, 94), 256u);
  capsule(canvas, x - 7.0f * s, y + 4.0f * s, x - 6.0f * s, y - 4.0f * s,
          1.0f * s, RGB(93, 129, 50), 256u);
  ellipse(canvas, x - 10.0f * s, y - 2.0f * s, 5.0f * s, 2.4f * s,
          RGB(114, 155, 58), 256u);
  ellipse(canvas, x - 2.0f * s, y - 4.0f * s, 5.0f * s, 2.6f * s,
          RGB(151, 174, 80), 256u);
  for (int i = 0; i < 3; ++i)
    oellipse(canvas, x + 16.0f * s, y + (19.5f - (float)i * 3.0f) * s, 8.0f * s,
             2.5f * s, s, RGB(246, 195, 78), RGB(154, 105, 44));
}

static void cover_card(h2_garden_guard_canvas_t* canvas,
                       h2_garden_guard_rect_t r, unsigned card) {
  static const uint16_t colors[] = {RGB(85, 126, 103), RGB(165, 132, 73),
                                    RGB(129, 105, 72)};
  const float s = r.w / (card == 1u ? 144.0f : 133.0f);
  const float mid = r.x + r.w * 0.5f, y1 = r.y + r.h;
  cover_panel(canvas, r, colors[card]);
  if (card == 0u)
    cover_book(canvas, mid, r.y + 34.0f * s, s);
  else if (card == 1u)
    cover_gate(canvas, mid, r.y + 40.0f * s, s);
  else
    cover_market(canvas, mid, r.y + 34.0f * s, s);
  const uint16_t band = card == 1u ? RGB(247, 202, 109) : RGB(56, 72, 53);
  trapezoid(canvas, y1 - 29.0f * s, r.x + 4.0f * s, r.x + r.w - 4.0f * s,
            y1 - 4.0f * s, r.x + 7.0f * s, r.x + r.w - 7.0f * s, band, 256u);
  fill_rect(canvas, r.x + 9.0f * s, y1 - 28.0f * s, r.x + r.w - 9.0f * s,
            y1 - 27.0f * s, RGB(255, 227, 149), 120u);
}

static void cover_native_label(h2_garden_guard_canvas_t* canvas,
                               const char* text, h2_garden_guard_rect_t r,
                               float center_y, uint16_t color) {
  const int32_t scale = fit_scale(text, r.w * 0.88f, r.w >= 86.0f ? 2 : 1);
  text_layer(
      canvas, text,
      (int32_t)(r.x +
                (r.w - (float)h2_garden_guard_text_width(text, scale)) * 0.5f),
      (int32_t)(center_y - (float)label_height(text, scale) * 0.5f), scale,
      color, 256u);
}

static void draw_title(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float s = fminf((float)l->width / 480.0f, (float)l->height / 320.0f);
  const float cx = (float)l->width * 0.5f;
  const float y = ((float)l->height - 320.0f * s) * 0.5f;
  const float left = cx - 240.0f * s;
  const uint16_t dark = RGB(21, 41, 32), cream = RGB(255, 231, 180);
  fill_rect(canvas, 0.0f, 0.0f, (float)l->width, (float)l->height, dark, 256u);
  draw_title_art(canvas, left, y + 8.0f * s, s);
  for (int i = 0; i < 31; ++i) {
    const float py = y + (8.0f + (float)i) * s;
    fill_rect(canvas, left, py, left + 480.0f * s, py + s, dark,
              (uint32_t)(31 - i) * 8u);
  }
  for (int i = 0; i < 37; ++i) {
    const float py = y + (200.0f + (float)i) * s;
    fill_rect(canvas, left, py, left + 480.0f * s, py + s, dark,
              (uint32_t)(i + 1) * 256u / 37u);
  }
  const h2_garden_guard_rect_t journal = h2_garden_guard_menu_tab(l, false, 0u);
  const h2_garden_guard_rect_t start =
      h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_START);
  const h2_garden_guard_rect_t shop = h2_garden_guard_menu_tab(l, false, 1u);
  cover_card(canvas, journal, 0u);
  cover_card(canvas, start, 1u);
  cover_card(canvas, shop, 2u);
  const h2_garden_guard_rect_t language =
      h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_LANGUAGE);
  const float lx = language.x, ly = language.y, lw = language.w,
              lh = language.h;
  capsule(canvas, lx + lw * 0.25f, y, lx + lw * 0.18f, ly + lh * 0.42f,
          .75f * s, RGB(214, 185, 125), 256u);
  capsule(canvas, lx + lw * 0.75f, y, lx + lw * 0.82f, ly + lh * 0.42f,
          .75f * s, RGB(214, 185, 125), 256u);
  trapezoid(canvas, ly + 6.0f * s, lx + 3.0f * s, lx + lw - 3.0f * s,
            ly + lh - 2.0f * s, lx + 6.0f * s, lx + lw - 6.0f * s,
            RGB(136, 94, 56), 256u);
  trapezoid(canvas, ly + 7.0f * s, lx + 5.0f * s, lx + lw - 5.0f * s,
            ly + lh - 4.0f * s, lx + 8.0f * s, lx + lw - 8.0f * s,
            RGB(226, 185, 114), 256u);
  if (canvas->external_cover_labels) return;
  if (game->language == H2_GARDEN_GUARD_LANG_ZH) {
    cover_type(canvas, &k_cover_title[0][0], 32u, cx, y + 26.0f * s, s, cream);
    cover_type(canvas, &k_cover_start[0][0], 18u, cx, y + 295.5f * s, s, dark);
  } else {
    const char* title = "GARDEN GUARD";
    const int32_t scale = fit_scale(title, 308.0f * s, text_scale(s, 4.0f));
    text_layer(
        canvas, title,
        (int32_t)(cx - 0.5f * (float)h2_garden_guard_text_width(title, scale)),
        (int32_t)(y + 26.0f * s - 3.5f * (float)scale), scale, cream, 256u);
    cover_native_label(canvas, "START GAME", start, y + 295.5f * s, dark);
  }
  cover_native_label(canvas, L("JOURNAL", "图鉴"), journal, y + 295.5f * s,
                     cream);
  cover_native_label(canvas, L("SHOP", "商店"), shop, y + 295.5f * s, cream);
  cover_native_label(
      canvas, language.w < 44.0f ? L("中文", "EN") : L("中文", "ENGLISH"),
      language, ly + lh * 0.57f, dark);
  if (game->language_unsaved)
    text_centered(canvas, L("LANGUAGE FOR THIS SESSION", "语言仅本次有效"), cx,
                  y + 43.0f * s, 1, cream, 256u);
}

static void draw_result(h2_garden_guard_canvas_t* canvas,
                        const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const h2_garden_guard_rect_t tab = h2_garden_guard_menu_tab(l, true, 0u);
  const float s = tab.w / 182.0f;
  const float cx = (float)l->width * 0.5f;
  const float y = tab.y - 278.0f * s;
  const uint16_t ivory = RGB(252, 233, 183);
  const bool won = game->phase == H2_GARDEN_GUARD_PHASE_WON;
  rounded_rect(canvas, cx - 218.0f * s, y + 65.0f * s, cx + 218.0f * s,
               y + 314.0f * s, 12.0f * s, RGB(20, 42, 33), 240u);
  const char* title = won ? L("GARDEN SAFE!", "庭院守住了")
                          : L("ZOMBIES GOT IN!", "僵尸闯进来了");
  text_centered(canvas, title, cx, y + 83.0f * s,
                fit_scale(title, 424.0f * s, text_scale(s, 4.0f)),
                won ? RGB(235, 211, 130) : RGB(255, 177, 133), 256u);
  char value[40];
  (void)snprintf(value, sizeof(value), L("RUN SCORE %u", "本局%u分"),
                 (unsigned)game->score);
  text_centered(canvas, value, cx, y + 135.0f * s,
                fit_scale(value, 420.0f * s, text_scale(s, 3.0f)), ivory, 256u);
  (void)snprintf(value, sizeof(value), L("TOTAL %u  BEST %u", "累计%u  最高%u"),
                 (unsigned)game->profile.total_score, (unsigned)game->best);
  text_centered(canvas, value, cx, y + 174.0f * s,
                fit_scale(value, 420.0f * s, text_scale(s, 2.0f)), ivory, 256u);
  (void)snprintf(value, sizeof(value),
                 s < 0.52f ? L("+%u COINS  %u", "+%u金币  余%u")
                           : L("+%u COINS  BALANCE %u", "+%u金币  余额%u"),
                 (unsigned)game->earned_coins, (unsigned)game->profile.coins);
  text_centered(canvas, value, cx, y + 208.0f * s,
                fit_scale(value, 420.0f * s, text_scale(s, 2.0f)),
                RGB(255, 206, 112), 256u);
  if (game->save_state != H2_GARDEN_GUARD_SAVE_READY ||
      game->language_unsaved) {
    const char* status = (game->save_state == H2_GARDEN_GUARD_SAVE_SESSION ||
                          game->language_unsaved)
                             ? L("SESSION ONLY", "仅本次有效")
                             : L("SAVE PENDING", "正在重试保存");
    text_centered(canvas, status, cx, y + 224.0f * s,
                  fit_scale(status, 420.0f * s, 1), ivory, 256u);
  }
  menu_button(
      canvas,
      h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_RESULT_HOME),
      L("MAIN MENU", "主菜单"), RGB(50, 93, 78), ivory);
  menu_tabs(canvas, game, true);
  if (prompt_visible(game)) {
    const h2_garden_guard_rect_t again =
        h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_RESULT_AGAIN);
    menu_button(canvas, again, L("PLAY AGAIN", "再玩一局"), RGB(247, 204, 113),
                RGB(29, 62, 49));
  }
}

static void draw_lane_plants(h2_garden_guard_canvas_t* canvas,
                             const h2_garden_guard_game_t* game, uint32_t lane,
                             float scale, bool animate) {
  for (uint32_t col = 0; col < COLS; ++col) {
    const h2_garden_guard_plant_t* plant = &game->plants[lane][col];
    if (plant->kind == H2_GARDEN_GUARD_PLANT_NONE) continue;
    float x, y;
    h2_garden_guard_field_point(&game->layout, lane, (float)col + 0.5f, &x, &y);
    const h2_garden_guard_plant_pose_t pose =
        animate ? h2_garden_guard_plant_pose(game, lane, col) : k_neutral_plant;
    draw_plant_kind(canvas, plant->kind, x, y, scale, plant->stage, pose,
                    false);
  }
}

/* Cache the parked carts. On result screens the plants rest under the panel. */
static void draw_pause(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const h2_garden_guard_rect_t center =
      h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_RESUME);
  const float s = center.w / 228.0f, cx = center.x + center.w * 0.5f;
  const float y = center.y - 106.0f * s;
  const uint16_t cream = RGB(247, 227, 177), ink = RGB(27, 51, 42);
  const uint16_t green = RGB(50, 93, 78), gold = RGB(249, 202, 110);
  fill_rect(canvas, 0.0f, 0.0f, (float)l->width, (float)l->height, ink, 158u);
  rounded_rect(canvas, cx - 188.0f * s, y + 51.0f * s, cx + 188.0f * s,
               y + 272.0f * s, 12.0f * s, RGB(24, 53, 43), 256u);
  if (game->menu == H2_GARDEN_GUARD_MENU_CONFIRM) {
    text_centered(canvas, L("LEAVE THIS RUN?", "放弃本局？"), cx, y + 82.0f * s,
                  text_scale(s, 3.0f), cream, 256u);
    text_centered(canvas, L("THIS RUN WILL END", "本局进度将丢失"), cx,
                  y + 126.0f * s, text_scale(s, 2.0f), cream, 256u);
    text_centered(canvas,
                  s < 0.52f
                      ? L("SAVED PROGRESS STAYS", "已存进度会保留")
                      : L("YOUR SAVED PROGRESS STAYS", "已保存的进度会保留"),
                  cx, y + 154.0f * s, text_scale(s, 1.0f), cream, 256u);
    menu_button(canvas,
                h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_CANCEL),
                L("CANCEL", "取消"), gold, ink);
    menu_button(canvas,
                h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_CONFIRM),
                L("LEAVE RUN", "放弃本局"), green, cream);
    return;
  }
  text_centered(canvas, L("PAUSED", "已暂停"), cx, y + 71.0f * s,
                text_scale(s, 3.0f), cream, 256u);
  menu_button(canvas, center, L("RESUME", "继续游戏"), gold, ink);
  menu_button(
      canvas,
      h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_PAUSE_LANGUAGE),
      s < 0.58f ? L("中文", "ENGLISH") : L("LANGUAGE / 中文", "语言 / ENGLISH"),
      green, cream);
  menu_button(canvas,
              h2_garden_guard_action_rect(l, H2_GARDEN_GUARD_ACTION_LEAVE),
              L("MAIN MENU", "主菜单"), green, cream);
  if (game->language_unsaved)
    text_centered(canvas, L("LANGUAGE FOR THIS SESSION", "语言仅本次有效"), cx,
                  y + 253.0f * s, 1, cream, 256u);
}

#ifdef __EMSCRIPTEN__
void h2_garden_guard_collect_menu_labels(h2_garden_guard_menu_labels_t* menu,
                                         const h2_garden_guard_game_t* game) {
  h2_garden_guard_canvas_t canvas = {.width = game->layout.width,
                                     .height = game->layout.height,
                                     .measure = true,
                                     .external_menu_labels = true,
                                     .menu_labels = menu};
  if (game->menu == H2_GARDEN_GUARD_MENU_JOURNAL) {
    draw_journal(&canvas, game);
    if (game->journal_card != 0u) draw_journal_focus(&canvas, game);
  } else if (game->menu == H2_GARDEN_GUARD_MENU_SHOP) {
    draw_shop(&canvas, game);
  }
}
#endif

static void draw_frozen_scene(h2_garden_guard_canvas_t* canvas,
                              const h2_garden_guard_game_t* game);

static void draw_field(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
  if (game->menu == H2_GARDEN_GUARD_MENU_JOURNAL) {
    draw_journal(canvas, game);
    return;
  }
  if (game->menu == H2_GARDEN_GUARD_MENU_SHOP) {
    draw_shop(canvas, game);
    return;
  }
  if (game->phase == H2_GARDEN_GUARD_PHASE_TITLE) {
    draw_title(canvas, game);
    return;
  }
  const h2_garden_guard_layout_t* l = &game->layout;
  field_clip(canvas, l);
  for (uint32_t lane = 0; lane < LANES; ++lane) {
    depth_mode(canvas, DEPTH_WRITE, lane);
    const float s = h2_garden_guard_lane_size(l, lane);
    if (game->carts[lane].state == H2_GARDEN_GUARD_CART_READY) {
      float x, y;
      h2_garden_guard_field_point(l, lane, -0.35f, &x, &y);
      draw_cart(canvas, x, y, s * 0.62f);
    }
    if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING)
      draw_lane_plants(canvas, game, lane, s, false);
  }
  depth_mode(canvas, DEPTH_OFF, 0u);
  full_clip(canvas);
  if (h2_garden_guard_paused(game)) {
    draw_frozen_scene(canvas, game);
    draw_pause(canvas, game);
  } else if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING)
    draw_result(canvas, game);
}

void h2_garden_guard_render_static(h2_garden_guard_canvas_t* canvas,
                                   const h2_garden_guard_game_t* game,
                                   h2_garden_guard_part_t part) {
  full_clip(canvas);
  depth_mode(canvas, DEPTH_OFF, 0u);
  if (part == H2_GARDEN_GUARD_PART_FIELD) {
    draw_field(canvas, game);
    return;
  }
  if (game->phase == H2_GARDEN_GUARD_PHASE_TITLE ||
      (game->menu != H2_GARDEN_GUARD_MENU_NONE &&
       !h2_garden_guard_paused(game)))
    return;
  if (part == H2_GARDEN_GUARD_PART_SUN)
    draw_sun_counter(canvas, game);
  else if (part == H2_GARDEN_GUARD_PART_STATUS)
    draw_toolbar_status(canvas, game);
  else
    draw_card(canvas, game, (unsigned)part - H2_GARDEN_GUARD_PART_CARD_FIRST);
}

/* ---- Moving scene -------------------------------------------------------- */

static uint32_t fade(uint32_t age, uint32_t life, uint32_t peak) {
  if (age >= life || life == 0u) return 0u;
  return peak * (life - age) / life;
}

/* Outline the lawn cell a dragged card would land on. */
static void draw_hover(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
  if (game->hover_lane < 0 || game->hover_col < 0) return;
  const h2_garden_guard_layout_t* l = &game->layout;
  const uint32_t lane = (uint32_t)game->hover_lane;
  const float u0 = (float)game->hover_col / (float)COLS;
  const float u1 = (float)(game->hover_col + 1) / (float)COLS;
  const float f_far = 1.0f - (float)lane / (float)LANES;
  const float f_near = 1.0f - (float)(lane + 1u) / (float)LANES;
  float tx0, ty, tx1, bx0, by, bx1;
  h2_garden_guard_ground_point(l, u0, f_far, &tx0, &ty);
  h2_garden_guard_ground_point(l, u1, f_far, &tx1, &ty);
  h2_garden_guard_ground_point(l, u0, f_near, &bx0, &by);
  h2_garden_guard_ground_point(l, u1, f_near, &bx1, &by);
  const uint16_t color = game->selected == H2_GARDEN_GUARD_CARD_SHOVEL
                             ? RGB(255, 120, 90)
                             : RGB(255, 252, 214);
  depth_mode(canvas, DEPTH_TEST, lane);
  trapezoid(canvas, ty, tx0, tx1, by, bx0, bx1, color, 80u);
  const float w = fmaxf(1.0f, 1.4f * l->unit);
  capsule(canvas, tx0, ty, tx1, ty, w, color, 220u);
  capsule(canvas, bx0, by, bx1, by, w, color, 220u);
  capsule(canvas, tx0, ty, bx0, by, w, color, 220u);
  capsule(canvas, tx1, ty, bx1, by, w, color, 220u);
  depth_mode(canvas, DEPTH_OFF, 0u);
}

static void paint_scene(h2_garden_guard_canvas_t* canvas,
                        const h2_garden_guard_game_t* game, bool frozen) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float u = l->unit;
  if (!frozen && game->menu != H2_GARDEN_GUARD_MENU_NONE) return;
  field_clip(canvas, l);
  draw_hover(canvas, game);
  // Compose all moving figures far to near; the map handles parked carts.
  for (uint32_t lane = 0; lane < LANES; ++lane) {
    if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING) break;
    depth_mode(canvas, DEPTH_TEST, lane);
    const float s = h2_garden_guard_lane_size(l, lane);
    draw_lane_plants(canvas, game, lane, s, true);
    const h2_garden_guard_cart_t* cart = &game->carts[lane];
    if (cart->state == H2_GARDEN_GUARD_CART_SWEEPING) {
      float x, y;
      h2_garden_guard_field_point(l, lane, cart->x, &x, &y);
      draw_cart(canvas, x, y, s * 0.62f);
    }
    for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i) {
      const h2_garden_guard_pest_t* pest = &game->pests[i];
      if ((!pest->active && pest->exit_ms == 0u) || pest->lane != lane)
        continue;
      float x, y;
      h2_garden_guard_field_point(l, lane, pest->x, &x, &y);
      if (pest->slow_ms != 0u && pest->active)
        ellipse(canvas, x, y, s * 0.36f, s * 0.13f, RGB(141, 223, 235), 165u);
      draw_zombie(canvas, x, y, s, pest);
    }
    for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PEAS; ++i) {
      const h2_garden_guard_pea_t* pea = &game->peas[i];
      if (!pea->active || pea->lane != lane) continue;
      float x, y;
      h2_garden_guard_field_point(l, lane, pea->x, &x, &y);
      const float r = s * 0.085f;
      ellipse(canvas, x, y, r * 0.9f, r * 0.3f, RGB(18, 40, 14), 80u);
      const float py = y - H2_GARDEN_GUARD_PEA_HEIGHT * s;
      odisc(canvas, x, py, r, rim_width(s),
            pea->frost ? RGB(140, 220, 238) : RGB(140, 222, 84),
            pea->frost ? RGB(53, 114, 159) : RGB(40, 104, 30));
      disc(canvas, x - r * 0.3f, py - r * 0.3f, r * 0.35f, RGB(220, 255, 190),
           200u);
    }
  }
  depth_mode(canvas, DEPTH_OFF, 0u);

  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PARTICLES; ++i) {
    const h2_garden_guard_particle_t* p = &game->particles[i];
    if (p->active)
      disc(canvas, p->x, p->y, p->size, p->color,
           fade(p->age_ms, p->life_ms, 256u));
  }

  const float sun_r = h2_garden_guard_sun_radius(l);
  const float spin = (float)(game->time_ms % 600000u) * 0.0008f;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_SUNS; ++i) {
    const h2_garden_guard_sun_t* sun = &game->suns[i];
    if (!sun->active) continue;
    /* Blink during the last two seconds before a resting sun fades. */
    if (!sun->collecting &&
        sun->age_ms + 2000u >= H2_GARDEN_GUARD_SUN_LIFE_MS &&
        (sun->age_ms / 160u) % 2u == 1u)
      continue;
    /* Falling suns slide out from under the toolbar; collected ones fly to
     * the counter over it. */
    if (sun->collecting)
      full_clip(canvas);
    else
      field_clip(canvas, l);
    draw_sun(canvas, sun->x, sun->y, sun->collecting ? sun_r * 0.8f : sun_r,
             spin, 256u);
  }
  full_clip(canvas);

  if (!frozen && game->banner_ms > 0u && game->banner[0] != '\0') {
    const uint32_t alpha =
        game->banner_ms >= 500u ? 256u : game->banner_ms * 256u / 500u;
    char localized[40];
    const char* banner = game->banner;
    if (game->language == H2_GARDEN_GUARD_LANG_ZH) {
      if (game->wave == 0u)
        banner = "准备迎战";
      else if (game->wave == H2_GARDEN_GUARD_WAVES)
        banner = "最后一波";
      else {
        (void)snprintf(localized, sizeof(localized), "第%u波",
                       (unsigned)game->wave);
        banner = localized;
      }
    }
    const int32_t scale =
        fit_scale(banner, l->field_w * 0.86f, text_scale(u, 5.0f));
    text_centered(canvas, banner, l->mid_x,
                  l->field_y + l->field_h * 0.45f - 3.5f * (float)scale, scale,
                  RGB(255, 238, 120), alpha);
  }
}

static void draw_frozen_scene(h2_garden_guard_canvas_t* canvas,
                              const h2_garden_guard_game_t* game) {
  paint_scene(canvas, game, true);
}

void h2_garden_guard_render_scene(h2_garden_guard_canvas_t* canvas,
                                  const h2_garden_guard_game_t* game) {
  if (game->menu == H2_GARDEN_GUARD_MENU_JOURNAL && game->journal_card != 0u) {
    draw_journal_focus(canvas, game);
    return;
  }
  paint_scene(canvas, game, false);
}
