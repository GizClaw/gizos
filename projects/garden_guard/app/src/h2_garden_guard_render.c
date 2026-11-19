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
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    const float ny = ((float)py + 0.5f - cy) * iry;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float nx = ((float)px + 0.5f - cx) * irx;
      const float d = sqrtf(nx * nx + ny * ny);
      const float cover = clampf((1.0f - d) * edge + 0.5f, 0.0f, 1.0f);
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
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    uint8_t* depth = depth_row(canvas, py);
    const float fy = (float)py + 0.5f;
    const float dy = fmaxf(fmaxf(y0 + radius - fy, fy - (y1 - radius)), 0.0f);
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float fx = (float)px + 0.5f;
      const float dx = fmaxf(fmaxf(x0 + radius - fx, fx - (x1 - radius)), 0.0f);
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

int32_t h2_garden_guard_text_width(const char* text, int32_t scale) {
  const size_t length = text != NULL ? strlen(text) : 0u;
  if (length == 0u || scale <= 0) return 0;
  return (int32_t)length * 6 * scale - scale;
}

static void text_layer(h2_garden_guard_canvas_t* canvas, const char* text,
                       int32_t x, int32_t y, int32_t scale, uint16_t color,
                       uint32_t alpha) {
  for (const char* c = text; *c != '\0'; ++c, x += 6 * scale) {
    const uint8_t* rows = glyph(*c);
    if (rows == NULL) continue;
    for (int32_t row = 0; row < 7; ++row)
      for (int32_t column = 0; column < 5; ++column)
        if (rows[row] & (0x10u >> column))
          fill_rect(canvas, (float)(x + column * scale),
                    (float)(y + row * scale), (float)(x + (column + 1) * scale),
                    (float)(y + (row + 1) * scale), color, alpha);
  }
}

/* Text with a soft drop shadow; (x, y) is the top-left corner. */
static void text(h2_garden_guard_canvas_t* canvas, const char* value, int32_t x,
                 int32_t y, int32_t scale, uint16_t color, uint32_t alpha) {
  const int32_t offset = scale / 2 > 0 ? scale / 2 : 1;
  text_layer(canvas, value, x + offset, y + offset, scale, RGB(20, 16, 8),
             alpha * 3u / 4u);
  text_layer(canvas, value, x, y, scale, color, alpha);
}

static void text_centered(h2_garden_guard_canvas_t* canvas, const char* value,
                          float cx, float y, int32_t scale, uint16_t color,
                          uint32_t alpha) {
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
/* The six joints and two moving face pieces share local coordinates. Fall
 * rotation is applied around the boots, so the exit stays in its lane. */
typedef struct zombie_brush {
  h2_garden_guard_canvas_t* canvas;
  float x, y, size, edge, cos_angle, sin_angle;
  uint16_t ink;
  uint32_t flash;
} zombie_brush_t;

static void zombie_point(const zombie_brush_t* brush, float x, float y,
                         float* px, float* py) {
  *px = brush->x + (x * brush->cos_angle - y * brush->sin_angle) * brush->size;
  *py = brush->y + (y * brush->cos_angle + x * brush->sin_angle) * brush->size;
}

static void zombie_limb(const zombie_brush_t* brush, float x0, float y0,
                        float x1, float y1, float r, uint16_t fill) {
  float ax, ay, bx, by;
  zombie_point(brush, x0, y0, &ax, &ay);
  zombie_point(brush, x1, y1, &bx, &by);
  ocapsule(brush->canvas, ax, ay, bx, by, r * brush->size, brush->edge,
           lighten(fill, brush->flash), brush->ink);
}

static void zombie_oval(const zombie_brush_t* brush, float x, float y, float rx,
                        float ry, uint16_t fill, bool outlined) {
  float px, py;
  zombie_point(brush, x, y, &px, &py);
  if (outlined)
    oellipse(brush->canvas, px, py, rx * brush->size, ry * brush->size,
             brush->edge, lighten(fill, brush->flash), brush->ink);
  else
    ellipse(brush->canvas, px, py, rx * brush->size, ry * brush->size,
            lighten(fill, brush->flash), 256u);
}

#define ZOMBIE_SHAPE(brush, color, ...)                                       \
  ART((brush)->canvas, (brush)->x, (brush)->y, (brush)->size,                 \
      (brush)->cos_angle, (brush)->sin_angle, lighten(color, (brush)->flash), \
      __VA_ARGS__)

#define ZOMBIE_ROUND(brush, color, ...)                             \
  ART_ROUND((brush)->canvas, (brush)->x, (brush)->y, (brush)->size, \
            (brush)->cos_angle, (brush)->sin_angle,                 \
            lighten(color, (brush)->flash), __VA_ARGS__)

/* The shoulder, elbow and wrist are one quadratic contour. Different spans
 * of the same path form the cloth sleeve and the exposed wrist. */
static void zombie_arm(const zombie_brush_t* brush, art_point_t shoulder,
                       art_point_t elbow, art_point_t wrist, float width,
                       uint16_t sleeve, uint16_t skin) {
  enum { samples = 7, sleeve_samples = 5 };
  art_point_t outline[2 * samples];
  art_point_t cloth[2 * sleeve_samples];
  for (unsigned i = 0; i < samples; ++i) {
    const float t = (float)i / (float)(samples - 1), u = 1.0f - t;
    const float x =
        u * u * shoulder.x + 2.0f * u * t * elbow.x + t * t * wrist.x;
    const float y =
        u * u * shoulder.y + 2.0f * u * t * elbow.y + t * t * wrist.y;
    const float dx = u * (elbow.x - shoulder.x) + t * (wrist.x - elbow.x);
    const float dy = u * (elbow.y - shoulder.y) + t * (wrist.y - elbow.y);
    const float length = fmaxf(0.028f, sqrtf(dx * dx + dy * dy));
    const float radius = width * (1.0f - t * 0.64f);
    const float nx = dy / length, ny = -dx / length;
    outline[i] = (art_point_t){x + radius * nx, y + radius * ny};
    outline[2 * samples - 1 - i] =
        (art_point_t){x - radius * nx, y - radius * ny};
    if (i < sleeve_samples) {
      const float r =
          i == sleeve_samples - 1 ? radius + 0.004f : radius * 0.89f;
      cloth[i] = (art_point_t){x + r * nx, y + r * ny};
      cloth[2 * sleeve_samples - 1 - i] = (art_point_t){x - r * nx, y - r * ny};
    }
  }
  art_round(brush->canvas, brush->x, brush->y, brush->size, brush->cos_angle,
            brush->sin_angle, lighten(skin, brush->flash), outline,
            2 * samples);
  art_round(brush->canvas, brush->x, brush->y, brush->size, brush->cos_angle,
            brush->sin_angle, lighten(sleeve, brush->flash), cloth,
            2 * sleeve_samples);
}

static void zombie_leg(const zombie_brush_t* brush, float stance, float start_y,
                       float stride, float lift, bool near, bool runner,
                       bool armored, uint16_t cloth) {
  const float foot = stance + stride;
  const float knee = stance + stride * 0.56f - 0.038f;
  const float knee_y = -0.225f - 0.39f * lift;
  const float shoe_y = -0.065f - lift;
  const uint16_t dungarees = near ? cloth : darken(cloth, 64u);
  const float cuff = runner ? 0.048f : (armored ? 0.087f : 0.068f);
  zombie_limb(brush, stance, start_y, knee, knee_y, cuff, dungarees);
  zombie_limb(brush, knee, knee_y, foot, shoe_y - 0.012f, cuff * 0.76f,
              dungarees);
  const uint16_t boot = near ? RGB(152, 104, 75) : RGB(105, 88, 76);
  const float toe = foot - (runner ? 0.152f : 0.157f);
  const float heel = foot + (armored ? 0.113f : 0.083f);
  ZOMBIE_ROUND(brush, brush->ink, {toe - 0.012f, shoe_y + 0.027f},
               {toe, shoe_y - 0.039f}, {foot - 0.052f, shoe_y - 0.058f},
               {foot - 0.027f, shoe_y - 0.10f},
               {heel - 0.019f, shoe_y - 0.073f}, {heel, shoe_y + 0.034f},
               {toe + 0.011f, shoe_y + 0.048f});
  ZOMBIE_ROUND(
      brush, boot, {toe + 0.019f, shoe_y + 0.023f},
      {toe + 0.021f, shoe_y - 0.017f}, {foot - 0.033f, shoe_y - 0.032f},
      {foot - 0.003f, shoe_y - 0.065f}, {heel - 0.031f, shoe_y - 0.047f},
      {heel - 0.026f, shoe_y + 0.019f});
  if (near) {
    zombie_oval(brush, toe + 0.063f, shoe_y - 0.011f, 0.046f, 0.020f,
                RGB(190, 135, 91), false);
    zombie_oval(brush, toe + 0.085f, shoe_y + 0.014f, 0.072f, 0.017f,
                runner ? RGB(232, 202, 153) : lighten(boot, 52u), false);
  }
}

static void zombie_head(const zombie_brush_t* brush, float x, float y,
                        float jaw, uint8_t kind, bool plated, uint16_t skin) {
  const bool runner = kind == H2_GARDEN_GUARD_PEST_RUNNER;
  const bool armored = kind == H2_GARDEN_GUARD_PEST_SHELLBACK;
  const float nape = armored ? 0.204f : runner ? 0.155f : 0.187f;
  const float crown = runner ? 0.211f : 0.173f;
  const float drop = jaw * 0.090f;
  // The nose and the near cheek are the forward edge of one silhouette.
  ZOMBIE_ROUND(brush, brush->ink, {x - 0.158f, y - 0.139f},
               {x + 0.028f, y - crown}, {x + nape, y - 0.110f},
               {x + nape + 0.023f, y + 0.035f}, {x + 0.115f, y + 0.150f},
               {x - 0.049f, y + 0.218f + drop}, {x - 0.202f, y + 0.168f + drop},
               {x - 0.234f, y + 0.102f + drop * 0.73f},
               {x - 0.195f, y + 0.067f}, {x - 0.294f, y + 0.025f},
               {x - 0.302f, y - 0.029f}, {x - 0.212f, y - 0.069f});
  ZOMBIE_ROUND(
      brush, skin, {x - 0.145f, y - 0.125f}, {x + 0.028f, y - crown + 0.018f},
      {x + nape - 0.019f, y - 0.095f}, {x + nape + 0.004f, y + 0.03f},
      {x + 0.103f, y + 0.127f}, {x - 0.041f, y + 0.196f + drop},
      {x - 0.183f, y + 0.151f + drop}, {x - 0.213f, y + 0.101f + drop * 0.73f},
      {x - 0.163f, y + 0.059f}, {x - 0.274f, y + 0.011f},
      {x - 0.274f, y - 0.021f}, {x - 0.196f, y - 0.053f});
  zombie_oval(brush, x + nape - 0.034f, y + 0.035f, 0.055f, 0.071f,
              darken(skin, 22u), true);
  zombie_oval(brush, x + nape - 0.029f, y + 0.033f, 0.029f, 0.036f,
              lighten(skin, 38u), false);
  zombie_oval(brush, x - 0.211f, y - 0.010f, 0.054f, 0.024f, lighten(skin, 61u),
              false);
  ZOMBIE_ROUND(brush, RGB(70, 60, 59), {x - 0.201f, y + 0.079f},
               {x - 0.116f, y + 0.084f}, {x + 0.020f, y + 0.061f},
               {x - 0.002f, y + 0.115f + drop}, {x - 0.104f, y + 0.142f + drop},
               {x - 0.188f, y + 0.112f + drop * 0.79f});
  zombie_oval(brush, x - 0.147f, y + 0.096f, 0.026f, 0.023f, RGB(245, 231, 178),
              false);
  // One open eye and a small brow give the gardener a puzzled side glance.
  const float eye_y = y - (armored ? 0.035f : 0.057f);
  zombie_oval(brush, x - 0.114f, eye_y, 0.074f, runner ? 0.065f : 0.078f,
              RGB(244, 235, 191), false);
  zombie_oval(brush, x - 0.144f, eye_y + 0.009f - 0.012f * jaw, 0.032f, 0.041f,
              RGB(54, 69, 54), false);
  zombie_oval(brush, x - 0.148f, eye_y - 0.013f, 0.012f, 0.012f,
              RGB(253, 244, 219), false);
  zombie_limb(brush, x - 0.164f, eye_y - 0.081f, x - 0.081f, eye_y - 0.091f,
              0.015f, darken(skin, 51u));
  if (armored) {
    // Repurposed enamel bowl. The scuffed lid remains after the vest breaks.
    ZOMBIE_ROUND(brush, brush->ink, {x - 0.239f, y - 0.112f},
                 {x - 0.196f, y - 0.194f}, {x - 0.091f, y - 0.256f},
                 {x + 0.082f, y - 0.265f}, {x + 0.201f, y - 0.189f},
                 {x + 0.239f, y - 0.069f}, {x + 0.107f, y - 0.064f});
    ZOMBIE_ROUND(brush, plated ? RGB(181, 209, 195) : RGB(108, 151, 132),
                 {x - 0.198f, y - 0.126f}, {x - 0.159f, y - 0.179f},
                 {x - 0.071f, y - 0.220f}, {x + 0.072f, y - 0.233f},
                 {x + 0.181f, y - 0.171f}, {x + 0.203f, y - 0.092f});
    zombie_limb(brush, x - 0.194f, y - 0.123f, x + 0.207f, y - 0.072f, 0.022f,
                RGB(106, 154, 144));
  } else if (runner) {
    ZOMBIE_ROUND(brush, brush->ink, {x - 0.187f, y - 0.097f},
                 {x - 0.122f, y - 0.191f}, {x - 0.012f, y - 0.215f},
                 {x + 0.142f, y - 0.184f}, {x + 0.276f, y - 0.104f},
                 {x + 0.23f, y - 0.064f}, {x + 0.119f, y - 0.08f},
                 {x + 0.024f, y - 0.107f});
    ZOMBIE_ROUND(brush, RGB(121, 142, 100), {x - 0.155f, y - 0.117f},
                 {x - 0.105f, y - 0.170f}, {x + 0.013f, y - 0.183f},
                 {x + 0.129f, y - 0.159f}, {x + 0.228f, y - 0.099f},
                 {x + 0.129f, y - 0.104f}, {x + 0.021f, y - 0.131f});
  } else {
    // Tilted canvas sun hat, with a roomy brim and a moss-green crown band.
    ZOMBIE_ROUND(brush, brush->ink, {x - 0.298f, y - 0.154f},
                 {x - 0.195f, y - 0.19f}, {x - 0.028f, y - 0.165f},
                 {x + 0.21f, y - 0.129f}, {x + 0.312f, y - 0.121f},
                 {x + 0.273f, y - 0.059f}, {x + 0.072f, y - 0.089f},
                 {x - 0.125f, y - 0.113f});
    ZOMBIE_ROUND(brush, RGB(210, 155, 86), {x - 0.261f, y - 0.155f},
                 {x - 0.188f, y - 0.17f}, {x - 0.028f, y - 0.148f},
                 {x + 0.21f, y - 0.115f}, {x + 0.29f, y - 0.111f},
                 {x + 0.258f, y - 0.080f}, {x + 0.07f, y - 0.104f},
                 {x - 0.122f, y - 0.128f});
    ZOMBIE_ROUND(brush, brush->ink, {x - 0.153f, y - 0.158f},
                 {x - 0.084f, y - 0.261f}, {x + 0.028f, y - 0.287f},
                 {x + 0.132f, y - 0.251f}, {x + 0.18f, y - 0.15f},
                 {x + 0.058f, y - 0.119f});
    ZOMBIE_ROUND(brush, RGB(217, 170, 97), {x - 0.115f, y - 0.162f},
                 {x - 0.055f, y - 0.236f}, {x + 0.03f, y - 0.253f},
                 {x + 0.104f, y - 0.224f}, {x + 0.151f, y - 0.161f},
                 {x + 0.056f, y - 0.147f});
    ZOMBIE_ROUND(brush, RGB(103, 133, 93), {x - 0.113f, y - 0.174f},
                 {x + 0.04f, y - 0.168f}, {x + 0.148f, y - 0.183f},
                 {x + 0.162f, y - 0.145f}, {x + 0.058f, y - 0.137f},
                 {x - 0.142f, y - 0.151f});
  }
}

/* The same figure can be posed for the title, a card or the field. */
static void draw_zombie(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                        float size, const h2_garden_guard_pest_t* pest) {
  const bool armored = pest->kind == H2_GARDEN_GUARD_PEST_SHELLBACK;
  const bool runner = pest->kind == H2_GARDEN_GUARD_PEST_RUNNER;
  const bool plated = armored && pest->armor > 0;
  const float s = size * (armored ? 1.07f : (runner ? 1.03f : 1.05f));
  const uint32_t period = runner ? 600u : (armored ? 1400u : 1050u);
  const float angle = (float)(pest->stride_ms % period) *
                      (2.0f * H2_GARDEN_GUARD_PI / (float)period);
  const float stride = sinf(angle), step = cosf(angle);
  const float bite = (float)pest->chew_ms / H2_GARDEN_GUARD_PEST_BITE_MS;
  const float open =
      pest->eating
          ? clampf(fminf(bite / 0.32f, (0.49f - bite) / 0.10f), 0.0f, 1.0f)
          : 0.0f;
  const float lunge =
      pest->eating
          ? 0.19f * clampf(1.0f - fabsf(bite - 0.48f) * 3.5f, 0.0f, 1.0f)
          : 0.0f;
  const float hit =
      pest->active ? (float)pest->flash_ms / H2_GARDEN_GUARD_PEST_HIT_MS : 0.0f;
  const float fall =
      pest->exit_ms == 0u
          ? 0.0f
          : 1.0f - (float)pest->exit_ms / H2_GARDEN_GUARD_PEST_EXIT_MS;
  const float tilt = 1.35f * fall * (2.0f - fall) +
                     (runner ? -0.15f : (armored ? 0.047f : -0.023f));
  const zombie_brush_t brush = {.canvas = canvas,
                                .x = gx + 0.14f * hit * s,
                                .y = gy + 0.05f * fall * s,
                                .size = s * (1.0f - 0.38f * fall * fall),
                                .edge = fmaxf(0.60f, s * 0.018f),
                                .cos_angle = cosf(tilt),
                                .sin_angle = sinf(tilt),
                                .ink = RGB(65, 78, 69),
                                .flash = (uint32_t)(165.0f * hit * hit)};
  const bool gardener = !armored && !runner;
  const float reach =
      pest->eating ? 0.07f : (runner ? 0.29f : (armored ? 0.17f : 0.215f));
  const float lift =
      pest->eating ? 0.0f : (runner ? 0.21f : (armored ? 0.10f : 0.14f));
  const uint16_t coat = armored
                            ? RGB(81, 130, 121)
                            : (runner ? RGB(181, 82, 59) : RGB(122, 104, 145));
  const uint16_t skin = RGB(154, 181, 144);
  const uint16_t pants = armored
                             ? RGB(79, 110, 108)
                             : (runner ? RGB(88, 115, 98) : RGB(88, 105, 129));
  const float belt = runner ? -0.48f : -0.43f;
  const float stance = armored ? 0.105f : 0.066f;
  ground_shadow(canvas, gx + fall * 0.26f * s, gy, s * (0.34f + fall * 0.19f));
  zombie_leg(&brush, stance, belt, stride * reach * (gardener ? 0.58f : 1.0f),
             fmaxf(0.0f, step) * lift * (gardener ? 0.20f : 1.0f), false,
             runner, armored, pants);
  zombie_leg(&brush, -stance, belt, -stride * reach, fmaxf(0.0f, -step) * lift,
             true, runner, armored, pants);

  const float sx = -(runner ? 0.17f : (armored ? 0.018f : 0.08f)) -
                   lunge * 0.75f + 0.08f * hit +
                   (gardener ? 0.035f * step : 0.0f);
  const float sy = (armored ? -0.62f : -0.71f) +
                   (pest->eating ? 0.055f : -0.034f * fabsf(step));
  const float hx = sx - (armored ? 0.067f : (runner ? 0.135f : 0.117f)) -
                   lunge * 0.78f + 0.032f * hit -
                   (gardener ? 0.047f * step : 0.0f);
  const float hy =
      sy - (runner ? 0.23f : 0.257f) + (pest->eating ? 0.037f : 0.0f);
  const float hand_dx =
      pest->eating ? 0.31f + lunge
                   : (runner ? 0.05f - stride * 0.25f : 0.26f - 0.10f * stride);
  const float hand_y = sy + (pest->eating ? 0.065f : 0.23f + 0.10f * stride);
  zombie_arm(&brush, (art_point_t){sx - 0.06f, sy + 0.028f},
             (art_point_t){sx - 0.18f, sy + 0.15f},
             (art_point_t){sx - 0.14f - hand_dx * 0.51f, hand_y - 0.10f},
             0.078f, darken(coat, 48u), darken(skin, 41u));
  const float shoulder = armored ? 0.266f : (runner ? 0.106f : 0.182f);
  ZOMBIE_ROUND(&brush, brush.ink, {sx - shoulder, sy - 0.016f},
               {sx - 0.06f, sy - 0.101f}, {sx + shoulder, sy - 0.044f},
               {sx + shoulder + 0.063f, sy + 0.12f}, {0.147f, belt + 0.035f},
               {-0.079f, belt + 0.057f}, {sx - shoulder + 0.015f, sy + 0.13f});
  ZOMBIE_SHAPE(&brush, coat, {sx - shoulder + 0.026f, sy + 0.001f},
               {sx - 0.05f, sy - 0.071f}, {sx + shoulder - 0.018f, sy - 0.018f},
               {sx + shoulder + 0.034f, sy + 0.12f}, {0.119f, belt + 0.012f},
               {-0.058f, belt + 0.026f}, {sx - shoulder + 0.042f, sy + 0.12f});
  if (plated) {
    zombie_oval(&brush, sx + 0.225f, sy + 0.028f, 0.148f, 0.093f,
                RGB(89, 138, 133), true);
    zombie_oval(&brush, sx + 0.207f, sy + 0.007f, 0.102f, 0.04f,
                RGB(178, 200, 164), false);
    ZOMBIE_SHAPE(&brush, RGB(46, 87, 87), {sx - 0.172f, sy - 0.024f},
                 {sx + 0.124f, sy - 0.009f}, {sx + 0.208f, sy + 0.101f},
                 {sx + 0.137f, sy + 0.253f}, {sx + 0.01f, sy + 0.284f},
                 {sx - 0.148f, sy + 0.212f});
    ZOMBIE_SHAPE(&brush, RGB(151, 194, 183), {sx - 0.142f, sy + 0.005f},
                 {sx + 0.108f, sy + 0.021f}, {sx + 0.17f, sy + 0.106f},
                 {sx + 0.111f, sy + 0.225f}, {sx + 0.015f, sy + 0.249f},
                 {sx - 0.122f, sy + 0.197f});
    ZOMBIE_SHAPE(&brush, RGB(217, 218, 171), {sx - 0.121f, sy + 0.025f},
                 {sx - 0.053f, sy + 0.044f}, {sx - 0.039f, sy + 0.123f},
                 {sx - 0.121f, sy + 0.092f});
  } else if (runner) {
    ZOMBIE_SHAPE(&brush, RGB(197, 76, 57), {hx - 0.09f, hy + 0.128f},
                 {sx + 0.22f, sy - 0.074f},
                 {sx + 0.486f, sy - 0.156f - 0.077f * stride},
                 {sx + 0.428f, sy - 0.037f - 0.077f * stride},
                 {sx + 0.279f, sy + 0.014f}, {hx - 0.034f, hy + 0.218f});
    ZOMBIE_SHAPE(&brush, RGB(244, 138, 75), {hx - 0.046f, hy + 0.15f},
                 {sx + 0.22f, sy - 0.06f},
                 {sx + 0.432f, sy - 0.129f - 0.077f * stride},
                 {sx + 0.265f, sy - 0.023f});
  } else {
    // Canvas work apron. A single bright sleeve balances the sagging shoulder.
    ZOMBIE_SHAPE(&brush, RGB(162, 136, 184), {sx - 0.065f, sy - 0.043f},
                 {sx + 0.042f, sy - 0.021f}, {sx + 0.139f, sy + 0.197f},
                 {sx + 0.016f, sy + 0.243f}, {sx - 0.056f, sy + 0.173f});
    ZOMBIE_SHAPE(&brush, RGB(209, 183, 136), {sx + 0.057f, sy - 0.038f},
                 {sx + 0.132f, sy - 0.018f}, {sx + 0.185f, sy + 0.081f},
                 {sx + 0.122f, sy + 0.107f});
    zombie_oval(&brush, sx + 0.025f, sy + 0.106f, 0.034f, 0.031f,
                RGB(230, 179, 104), false);
  }
  const float elbow =
      sx + (runner ? 0.13f + 0.11f * stride : 0.075f - 0.07f * stride);
  const float wrist = sx - hand_dx;
  zombie_arm(&brush, (art_point_t){sx + shoulder * 0.64f, sy + 0.055f},
             (art_point_t){elbow, sy + (pest->eating ? 0.14f : 0.29f)},
             (art_point_t){wrist, hand_y}, armored ? 0.105f : 0.09f,
             gardener ? RGB(196, 172, 124) : lighten(coat, 29u), skin);
  ZOMBIE_ROUND(
      &brush, lighten(skin, 32u), {wrist + 0.029f, hand_y - 0.023f},
      {wrist - 0.049f, hand_y - 0.046f}, {wrist - 0.114f, hand_y - 0.018f},
      {wrist - 0.108f, hand_y + 0.037f}, {wrist - 0.053f, hand_y + 0.065f},
      {wrist + 0.002f, hand_y + 0.045f});
  zombie_head(&brush, hx, hy, open, pest->kind, plated, skin);
}

void h2_garden_guard_render_zombie_portrait(h2_garden_guard_canvas_t* canvas,
                                            h2_garden_guard_pest_kind_t kind,
                                            float x, float ground_y,
                                            float size) {
  if ((unsigned)kind >= H2_GARDEN_GUARD_PEST_KIND_COUNT) return;
  const h2_garden_guard_pest_t zombie = {
      .kind = (uint8_t)kind, .active = true, .armor = 1};
  draw_zombie(canvas, x, ground_y, size, &zombie);
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
  if (part == H2_GARDEN_GUARD_PART_TOOLBAR) {
    out->values[0] = game->sun;
    out->values[1] = (uint32_t)(game->selected + 1);
    for (unsigned i = 0; i < H2_GARDEN_GUARD_CARD_COUNT; ++i) {
      const uint32_t cost =
          h2_garden_guard_card_cost((h2_garden_guard_card_t)i);
      out->values[2u + i] = (game->sun >= cost ? 1u : 0u) |
                            cooldown_step(game, i) << 1 |
                            (game->card_denied_ms[i] != 0u ? 1u : 0u) << 8;
    }
    out->values[8] = game->wave;
    out->values[9] = progress_step(game);
    out->values[10] = game->profile.owned;
    out->values[11] = game->sighting_ms != 0u ? 1u + game->sighting_kind : 0u;
    out->values[12] = game->menu;
    return;
  }
  uint32_t carts = 0;
  for (unsigned lane = 0; lane < LANES; ++lane)
    if (game->carts[lane].state == H2_GARDEN_GUARD_CART_READY)
      carts |= 1u << lane;
  out->values[1] = carts;
  if (game->phase == H2_GARDEN_GUARD_PHASE_PLAYING) return;
  out->values[0] = game->plant_revision;
  out->values[2] = game->score;
  out->values[3] = game->best;
  out->values[4] = game->new_best ? 1u : 0u;
  out->values[5] = prompt_visible(game) ? 1u : 0u;
  out->values[6] = game->kills;
  out->values[7] = game->menu;
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
      (void)snprintf(value, sizeof(value), "LOCK");
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

static const struct {
  const char* name;
  const char* trait;
  const char* counter;
} k_zombie_cards[H2_GARDEN_GUARD_PEST_KIND_COUNT] = {
    {"GARDENER", "DRAGS ONE FOOT", "KEEP SHOOTERS BEHIND WALLS"},
    {"SHELLBACK", "PLATE TAKES THE FIRST HITS", "STACK SHOOTERS IN ITS LANE"},
    {"RUNNER", "FAST BUT FRAGILE", "BLOCK IT OR USE FROST"},
};

static void draw_toolbar(h2_garden_guard_canvas_t* canvas,
                         const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float u = l->unit;
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
  for (unsigned i = 0; i < H2_GARDEN_GUARD_CARD_COUNT; ++i)
    draw_card(canvas, game, i);

  /* Wave label and progress toward the end of the last wave. */
  const float pad = l->card_y;
  const float x0 =
      l->card_x[H2_GARDEN_GUARD_CARD_COUNT - 1] + l->card_w + 3.0f * pad + 2.0f;
  const float x1 = (float)l->width - 2.0f * pad - 2.0f;
  if (x1 - x0 < 24.0f * u) return;
  if (game->wave == 0u)
    (void)snprintf(value, sizeof(value), "READY");
  else
    (void)snprintf(value, sizeof(value), "WAVE %u/%u", (unsigned)game->wave,
                   (unsigned)H2_GARDEN_GUARD_WAVES);
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
  if (game->sighting_ms != 0u) {
    const char* sighting = "NEW JOURNAL CARD";
    text_centered(canvas, sighting, (x0 + x1) * 0.5f,
                  bar_y0 + (bar_y1 - bar_y0 - 7.0f) * 0.5f,
                  fit_scale(sighting, x1 - x0, 1), RGB(255, 218, 119), 256u);
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
      const uint8_t* data = &k_title_art[k_title_row_offsets[sy]];
      int32_t sx = 0;
      while (sx < TITLE_ART_WIDTH) {
        const uint8_t token = *data++;
        if ((token & 128u) == 0u) {
          line[sx++] = token;
        } else {
          const size_t count = (token & 127u) + 3u;
          memset(&line[sx], *data++, count);
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
  const int32_t width = h2_garden_guard_text_width(label, scale);
  text_layer(canvas, label, (int32_t)(r.x + (r.w - (float)width) * 0.5f),
             (int32_t)(r.y + (r.h - (float)(scale * 7)) * 0.5f), scale, ink,
             256u);
}

static void menu_tabs(h2_garden_guard_canvas_t* canvas,
                      const h2_garden_guard_layout_t* l, bool result) {
  menu_button(canvas, h2_garden_guard_menu_tab(l, result, 0u), "JOURNAL",
              RGB(50, 93, 78), RGB(253, 235, 182));
  menu_button(canvas, h2_garden_guard_menu_tab(l, result, 1u), "SHOP",
              RGB(50, 93, 78), RGB(253, 235, 182));
}

static void draw_collection(h2_garden_guard_canvas_t* canvas,
                            const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const bool shop = game->menu == H2_GARDEN_GUARD_MENU_SHOP;
  const h2_garden_guard_rect_t back = h2_garden_guard_menu_back(l);
  const float s = back.w / 80.0f;
  const float cx = (float)l->width * 0.5f;
  const uint16_t paper = RGB(235, 216, 173);
  const uint16_t ink = RGB(35, 61, 51);
  const uint16_t muted = RGB(87, 111, 88);
  const uint16_t light = RGB(244, 226, 176);
  fill_rect(canvas, 0.0f, 0.0f, (float)l->width, (float)l->height,
            RGB(19, 39, 35), 256u);
  menu_button(canvas, back, "BACK", RGB(48, 88, 76), light);
  text_centered(canvas, shop ? "PLANT SHOP" : "FIELD JOURNAL", cx,
                back.y + 6.0f * s,
                fit_scale(shop ? "PLANT SHOP" : "FIELD JOURNAL", 252.0f * s,
                          text_scale(s, 3.0f)),
                light, 256u);
  char label[40];
  if (shop) {
    (void)snprintf(label, sizeof(label), "%u COINS",
                   (unsigned)game->profile.coins);
  } else {
    unsigned seen = 0;
    for (unsigned k = 0; k < H2_GARDEN_GUARD_PEST_KIND_COUNT; ++k)
      seen += (game->profile.seen >> k) & 1u;
    (void)snprintf(label, sizeof(label), "%u/%u SEEN", seen,
                   (unsigned)H2_GARDEN_GUARD_PEST_KIND_COUNT);
  }
  text_centered(canvas, label, cx, back.y + 39.0f * s,
                fit_scale(label, 260.0f * s, text_scale(s, 2.0f)),
                RGB(240, 197, 103), 256u);
  for (unsigned i = 0; i < (shop ? 2u : H2_GARDEN_GUARD_PEST_KIND_COUNT); ++i) {
    const h2_garden_guard_rect_t r = h2_garden_guard_menu_row(l, i);
    const bool seen = shop || (game->profile.seen & (1u << i)) != 0u;
    rounded_rect(canvas, r.x, r.y, r.x + r.w, r.y + r.h, 8.0f * s,
                 seen ? paper : RGB(48, 75, 66), 256u);
    rounded_rect(canvas, r.x + 5.0f * s, r.y + 5.0f * s, r.x + 79.0f * s,
                 r.y + r.h - 5.0f * s, 5.0f * s,
                 seen ? RGB(160, 177, 127) : RGB(56, 90, 76), 256u);
    if (shop) {
      const uint8_t kind =
          i == 0u ? H2_GARDEN_GUARD_PLANT_FROST : H2_GARDEN_GUARD_PLANT_BRAMBLE;
      draw_plant_kind(canvas, kind, r.x + 43.0f * s, r.y + 52.0f * s, 42.0f * s,
                      0u, k_neutral_plant, false);
    } else if (seen) {
      h2_garden_guard_render_zombie_portrait(
          canvas, (h2_garden_guard_pest_kind_t)i, r.x + 46.0f * s,
          r.y + 52.0f * s, 37.0f * s);
    } else {
      text_centered(canvas, "?", r.x + 43.0f * s, r.y + 18.0f * s,
                    text_scale(s, 3.0f), RGB(159, 185, 141), 256u);
    }
    const char* name = shop   ? (i == 0u ? "FROSTBELL" : "BRAMBLE")
                       : seen ? k_zombie_cards[i].name
                              : "UNDISCOVERED";
    const char* trait =
        shop ? (i == 0u ? "SLOWS WITH ICY SHOTS" : "THORNS HURT BITING ZOMBIES")
        : seen ? k_zombie_cards[i].trait
               : "MEET THIS ZOMBIE IN PLAY";
    const char* hint = shop ? (i == 0u ? "125 SUN TO PLANT" : "75 SUN TO PLANT")
                       : seen ? k_zombie_cards[i].counter
                              : NULL;
    if (s < 0.52f) {
      if (shop) {
        trait = i == 0u ? "SLOWS" : "THORN HITS";
        hint = i == 0u ? "125 SUN" : "75 SUN";
      } else if (seen) {
        static const char* const compact_traits[] = {"DRAGS ONE FOOT", "PLATED",
                                                     "FAST, FRAGILE"};
        static const char* const compact_hints[] = {
            "USE WALLS", "STACK SHOOTERS", "BLOCK OR FROST"};
        trait = compact_traits[i];
        hint = compact_hints[i];
      } else {
        trait = "MEET IN PLAY";
      }
    }
    const float x = r.x + 92.0f * s;
    const float available = (shop ? 226.0f : 335.0f) * s;
    text_layer(canvas, name, (int32_t)x, (int32_t)(r.y + 10.0f * s),
               fit_scale(name, available, text_scale(s, 2.0f)),
               seen ? ink : light, 256u);
    text_layer(canvas, trait, (int32_t)x, (int32_t)(r.y + 31.0f * s),
               fit_scale(trait, available, 1), seen ? ink : light, 256u);
    if (hint != NULL)
      text_layer(canvas, hint, (int32_t)x, (int32_t)(r.y + 45.0f * s),
                 fit_scale(hint, available, 1), muted, 256u);
    if (shop) {
      const uint32_t price = h2_garden_guard_offer_price(i);
      const bool owned =
          (game->profile.owned & (i == 0u ? H2_GARDEN_GUARD_FROST_BIT
                                          : H2_GARDEN_GUARD_BRAMBLE_BIT)) != 0u;
      const bool enough = game->profile.coins >= price;
      (void)snprintf(label, sizeof(label),
                     owned    ? "OWNED"
                     : enough ? "BUY %u"
                              : "NEED %u",
                     (unsigned)price);
      const h2_garden_guard_rect_t buy = {r.x + r.w * 0.74f + 7.0f * s,
                                          r.y + 15.0f * s,
                                          r.w * 0.26f - 14.0f * s, 32.0f * s};
      menu_button(canvas, buy, label,
                  owned    ? RGB(126, 162, 120)
                  : enough ? RGB(252, 199, 91)
                           : RGB(182, 168, 138),
                  ink);
    }
  }
  if (shop) {
    const h2_garden_guard_rect_t row = h2_garden_guard_menu_row(l, 2u);
    (void)snprintf(label, sizeof(label), "TOTAL SCORE %u",
                   (unsigned)game->profile.total_score);
    text_centered(canvas, label, cx, row.y + 9.0f * s,
                  fit_scale(label, 420.0f * s, text_scale(s, 2.0f)), light,
                  256u);
    const char* hint = s < 0.52f ? "PLANT KILLS EARN COINS"
                                 : "PLANT DEFEATS EARN COINS AT THE END";
    text_centered(canvas, hint, cx, row.y + 38.0f * s,
                  fit_scale(hint, 435.0f * s, 1), light, 256u);
  }
  if (game->save_state != H2_GARDEN_GUARD_SAVE_READY) {
    const char* status = game->save_state == H2_GARDEN_GUARD_SAVE_SESSION
                             ? "SESSION ONLY"
                             : "SAVE PENDING";
    text_centered(canvas, status, cx, back.y + 289.0f * s,
                  fit_scale(status, 420.0f * s, 1), RGB(244, 184, 114), 256u);
  }
}

static void draw_title(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float s =
      fminf((float)l->width / TITLE_ART_WIDTH, (float)l->height / 320.0f);
  const float cx = (float)l->width * 0.5f;
  const float y = ((float)l->height - 320.0f * s) * 0.5f;
  const uint16_t cover_ink = RGB(20, 39, 35);
  fill_rect(canvas, 0.0f, 0.0f, (float)l->width, (float)l->height, cover_ink,
            256u);
  draw_title_art(canvas, cx - 0.5f * TITLE_ART_WIDTH * s, y + 40.0f * s, s);

  const char* title = "GARDEN GUARD";
  const int32_t name_scale = fit_scale(title, 454.0f * s, text_scale(s, 3.9f));
  text_centered(canvas, title, cx, y + 17.0f * s - 1.0f * (float)name_scale,
                name_scale, RGB(255, 232, 164), 256u);

  const char* action = "TAP TO START";
  const int32_t action_scale =
      fit_scale(action, 226.0f * s, text_scale(s, 2.0f));
  rounded_rect(canvas, cx - 92.0f * s, y + 264.0f * s, cx + 92.0f * s,
               y + 298.0f * s, 9.0f * s, cover_ink, 256u);
  rounded_rect(canvas, cx - 89.0f * s, y + 267.0f * s, cx + 89.0f * s,
               y + 294.0f * s, 7.0f * s, RGB(250, 209, 112), 256u);
  text_centered(canvas, action, cx, y + 280.5f * s - 3.5f * (float)action_scale,
                action_scale, cover_ink, 256u);
  const char* hint =
      s < 0.52f ? "COLLECT SUN AND PLANT" : "COLLECT SUN, PICK A CARD, PLANT";
  text_centered(canvas, hint, cx, y + 304.0f * s,
                fit_scale(hint, 460.0f * s, 1), RGB(211, 219, 186), 256u);
  menu_tabs(canvas, l, false);
}

static void draw_result(h2_garden_guard_canvas_t* canvas,
                        const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const h2_garden_guard_rect_t tab = h2_garden_guard_menu_tab(l, true, 0u);
  const float s = tab.w / 108.0f;
  const float cx = (float)l->width * 0.5f;
  const float y = tab.y - 278.0f * s;
  const uint16_t ivory = RGB(252, 233, 183);
  const bool won = game->phase == H2_GARDEN_GUARD_PHASE_WON;
  rounded_rect(canvas, cx - 218.0f * s, y + 65.0f * s, cx + 218.0f * s,
               y + 314.0f * s, 12.0f * s, RGB(20, 42, 33), 240u);
  const char* title = won ? "GARDEN SAFE!" : "ZOMBIES GOT IN!";
  text_centered(canvas, title, cx, y + 83.0f * s,
                fit_scale(title, 424.0f * s, text_scale(s, 4.0f)),
                won ? RGB(235, 211, 130) : RGB(255, 177, 133), 256u);
  char value[40];
  (void)snprintf(value, sizeof(value), "RUN SCORE %u", (unsigned)game->score);
  text_centered(canvas, value, cx, y + 135.0f * s,
                fit_scale(value, 420.0f * s, text_scale(s, 3.0f)), ivory, 256u);
  (void)snprintf(value, sizeof(value), "TOTAL %u  BEST %u",
                 (unsigned)game->profile.total_score, (unsigned)game->best);
  text_centered(canvas, value, cx, y + 174.0f * s,
                fit_scale(value, 420.0f * s, text_scale(s, 2.0f)), ivory, 256u);
  (void)snprintf(value, sizeof(value),
                 s < 0.52f ? "+%u COINS  %u" : "+%u COINS  BALANCE %u",
                 (unsigned)game->earned_coins, (unsigned)game->profile.coins);
  text_centered(canvas, value, cx, y + 208.0f * s,
                fit_scale(value, 420.0f * s, text_scale(s, 2.0f)),
                RGB(255, 206, 112), 256u);
  if (game->save_state != H2_GARDEN_GUARD_SAVE_READY) {
    const char* status = game->save_state == H2_GARDEN_GUARD_SAVE_SESSION
                             ? "SESSION ONLY"
                             : "SAVE PENDING";
    text_centered(canvas, status, cx, y + 246.0f * s,
                  fit_scale(status, 420.0f * s, 1), ivory, 256u);
  }
  menu_tabs(canvas, l, true);
  if (prompt_visible(game)) {
    const h2_garden_guard_rect_t again = {cx - 92.0f * s, tab.y, 184.0f * s,
                                          tab.h};
    menu_button(canvas, again, "PLAY AGAIN", RGB(247, 204, 113),
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
static void draw_field(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
  if (game->menu != H2_GARDEN_GUARD_MENU_NONE) {
    draw_collection(canvas, game);
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
  if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING) draw_result(canvas, game);
}

void h2_garden_guard_render_static(h2_garden_guard_canvas_t* canvas,
                                   const h2_garden_guard_game_t* game,
                                   h2_garden_guard_part_t part) {
  full_clip(canvas);
  depth_mode(canvas, DEPTH_OFF, 0u);
  if (part == H2_GARDEN_GUARD_PART_TOOLBAR) {
    if (game->phase != H2_GARDEN_GUARD_PHASE_TITLE &&
        game->menu == H2_GARDEN_GUARD_MENU_NONE)
      draw_toolbar(canvas, game);
  } else {
    draw_field(canvas, game);
  }
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

void h2_garden_guard_render_scene(h2_garden_guard_canvas_t* canvas,
                                  const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float u = l->unit;
  if (game->menu != H2_GARDEN_GUARD_MENU_NONE) return;
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

  if (game->banner_ms > 0u && game->banner[0] != '\0') {
    const uint32_t alpha =
        game->banner_ms >= 500u ? 256u : game->banner_ms * 256u / 500u;
    const int32_t scale =
        fit_scale(game->banner, l->field_w * 0.86f, text_scale(u, 5.0f));
    text_centered(canvas, game->banner, l->mid_x,
                  l->field_y + l->field_h * 0.45f - 3.5f * (float)scale, scale,
                  RGB(255, 238, 120), alpha);
  }
}
