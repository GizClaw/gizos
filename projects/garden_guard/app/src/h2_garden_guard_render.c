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
 * nearer lane owns, and mostly opaque plant pixels claim their lane. */
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

static void draw_sunflower(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                           float s, uint32_t stage) {
  const uint32_t dim = stage * 46u;
  const float line = rim_width(s);
  const uint16_t rim = RGB(34, 72, 26);
  ground_shadow(canvas, gx, gy, 0.3f * s);
  ocapsule(canvas, gx, gy - 0.02f * s, gx, gy - 0.62f * s, 0.045f * s, line,
           darken(RGB(80, 154, 52), dim), rim);
  oellipse(canvas, gx - 0.15f * s, gy - 0.22f * s, 0.15f * s, 0.065f * s, line,
           darken(RGB(92, 176, 62), dim), rim);
  oellipse(canvas, gx + 0.15f * s, gy - 0.35f * s, 0.14f * s, 0.06f * s, line,
           darken(RGB(92, 176, 62), dim), rim);
  const float hx = gx, hy = gy - 0.74f * s;
  for (unsigned k = 0; k < 12u; ++k) {
    const float a = (float)k * (2.0f * H2_GARDEN_GUARD_PI / 12.0f);
    disc(canvas, hx + cosf(a) * 0.22f * s, hy + sinf(a) * 0.22f * s,
         0.1f * s + line, RGB(150, 84, 10), 256u);
  }
  for (unsigned k = 0; k < 12u; ++k) {
    const float a = (float)k * (2.0f * H2_GARDEN_GUARD_PI / 12.0f);
    disc(canvas, hx + cosf(a) * 0.22f * s, hy + sinf(a) * 0.22f * s, 0.1f * s,
         darken(RGB(244, 170, 30), dim), 256u);
  }
  for (unsigned k = 0; k < 12u; ++k) {
    const float a = ((float)k + 0.5f) * (2.0f * H2_GARDEN_GUARD_PI / 12.0f);
    disc(canvas, hx + cosf(a) * 0.17f * s, hy + sinf(a) * 0.17f * s, 0.08f * s,
         darken(RGB(255, 216, 64), dim), 256u);
  }
  odisc(canvas, hx, hy, 0.15f * s, line, darken(RGB(128, 78, 34), dim),
        RGB(70, 40, 14));
  disc(canvas, hx, hy + 0.01f * s, 0.11f * s, darken(RGB(152, 96, 42), dim),
       256u);
  const uint16_t ink = RGB(36, 20, 8);
  disc(canvas, hx - 0.055f * s, hy - 0.03f * s, 0.03f * s, ink, 256u);
  disc(canvas, hx + 0.055f * s, hy - 0.03f * s, 0.03f * s, ink, 256u);
  disc(canvas, hx - 0.047f * s, hy - 0.04f * s, 0.011f * s, RGB(255, 250, 230),
       256u);
  disc(canvas, hx + 0.063f * s, hy - 0.04f * s, 0.011f * s, RGB(255, 250, 230),
       256u);
  capsule(canvas, hx - 0.06f * s, hy + 0.035f * s, hx - 0.02f * s,
          hy + 0.065f * s, 0.016f * s, ink, 230u);
  capsule(canvas, hx - 0.02f * s, hy + 0.065f * s, hx + 0.02f * s,
          hy + 0.065f * s, 0.016f * s, ink, 230u);
  capsule(canvas, hx + 0.02f * s, hy + 0.065f * s, hx + 0.06f * s,
          hy + 0.035f * s, 0.016f * s, ink, 230u);
  disc(canvas, hx - 0.095f * s, hy + 0.035f * s, 0.028f * s, RGB(236, 110, 90),
       110u);
  disc(canvas, hx + 0.095f * s, hy + 0.035f * s, 0.028f * s, RGB(236, 110, 90),
       110u);
}

static void draw_shooter(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                         float s, uint32_t stage) {
  const uint32_t dim = stage * 46u;
  const float line = rim_width(s);
  const uint16_t rim = RGB(30, 66, 24);
  const uint16_t leaf = darken(RGB(76, 156, 54), dim);
  ground_shadow(canvas, gx, gy, 0.28f * s);
  ocapsule(canvas, gx, gy - 0.03f * s, gx - 0.24f * s, gy - 0.1f * s,
           0.055f * s, line, leaf, rim);
  ocapsule(canvas, gx, gy - 0.03f * s, gx + 0.22f * s, gy - 0.08f * s,
           0.05f * s, line, leaf, rim);
  ocapsule(canvas, gx, gy - 0.04f * s, gx - 0.02f * s, gy - 0.45f * s,
           0.055f * s, line, darken(RGB(84, 160, 54), dim), rim);
  const float hx = gx - 0.03f * s, hy = gy - 0.62f * s;
  ocapsule(canvas, hx, hy, hx + 0.27f * s, hy - 0.01f * s, 0.1f * s, line,
           darken(RGB(100, 190, 64), dim), rim);
  odisc(canvas, hx + 0.3f * s, hy - 0.01f * s, 0.12f * s, line,
        darken(RGB(86, 166, 54), dim), rim);
  disc(canvas, hx + 0.32f * s, hy - 0.01f * s, 0.065f * s, RGB(24, 56, 20),
       256u);
  odisc(canvas, hx, hy, 0.23f * s, line, darken(RGB(120, 210, 76), dim), rim);
  disc(canvas, hx - 0.08f * s, hy - 0.09f * s, 0.07f * s, RGB(230, 255, 210),
       90u);
  odisc(canvas, hx + 0.06f * s, hy - 0.07f * s, 0.065f * s, line,
        RGB(250, 252, 240), rim);
  disc(canvas, hx + 0.085f * s, hy - 0.07f * s, 0.035f * s, RGB(20, 24, 16),
       256u);
  ocapsule(canvas, hx - 0.08f * s, hy - 0.2f * s, hx - 0.24f * s,
           hy - 0.32f * s, 0.045f * s, line, leaf, rim);
}

static void draw_pumpkin(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                         float s, uint32_t stage) {
  const uint32_t dim = stage * 30u;
  const float line = rim_width(s);
  const uint16_t rim = RGB(120, 52, 10);
  const float cy = gy - 0.3f * s;
  ground_shadow(canvas, gx, gy, 0.42f * s);
  oellipse(canvas, gx - 0.17f * s, cy, 0.21f * s, 0.27f * s, line,
           darken(RGB(216, 110, 26), dim), rim);
  oellipse(canvas, gx + 0.17f * s, cy, 0.21f * s, 0.27f * s, line,
           darken(RGB(216, 110, 26), dim), rim);
  oellipse(canvas, gx, cy - 0.01f * s, 0.23f * s, 0.3f * s, line,
           darken(RGB(242, 138, 36), dim), rim);
  ellipse(canvas, gx - 0.07f * s, cy - 0.09f * s, 0.05f * s, 0.15f * s,
          RGB(255, 196, 120), 110u);
  ellipse(canvas, gx - 0.24f * s, cy - 0.05f * s, 0.035f * s, 0.12f * s,
          RGB(255, 180, 100), 80u);
  ocapsule(canvas, gx, cy - 0.27f * s, gx + 0.05f * s, cy - 0.4f * s,
           0.045f * s, line, RGB(112, 78, 36), RGB(60, 40, 16));
  oellipse(canvas, gx - 0.1f * s, cy - 0.32f * s, 0.08f * s, 0.035f * s, line,
           RGB(84, 160, 54), RGB(30, 66, 24));
  const uint16_t ink = RGB(70, 30, 6);
  ellipse(canvas, gx - 0.09f * s, cy - 0.03f * s, 0.04f * s, 0.055f * s, ink,
          256u);
  ellipse(canvas, gx + 0.09f * s, cy - 0.03f * s, 0.04f * s, 0.055f * s, ink,
          256u);
  capsule(canvas, gx - 0.15f * s, cy - 0.12f * s, gx - 0.05f * s,
          cy - 0.09f * s, 0.018f * s, ink, 240u);
  capsule(canvas, gx + 0.15f * s, cy - 0.12f * s, gx + 0.05f * s,
          cy - 0.09f * s, 0.018f * s, ink, 240u);
  capsule(canvas, gx - 0.06f * s, cy + 0.1f * s, gx + 0.06f * s, cy + 0.1f * s,
          0.02f * s, ink, 230u);
  if (stage >= 1u) {
    capsule(canvas, gx + 0.1f * s, cy - 0.24f * s, gx + 0.17f * s,
            cy - 0.1f * s, 0.022f * s, RGB(92, 40, 10), 240u);
    capsule(canvas, gx + 0.17f * s, cy - 0.1f * s, gx + 0.27f * s,
            cy - 0.04f * s, 0.022f * s, RGB(92, 40, 10), 240u);
  }
  if (stage >= 2u) {
    capsule(canvas, gx - 0.3f * s, cy + 0.04f * s, gx - 0.16f * s,
            cy + 0.12f * s, 0.024f * s, RGB(92, 40, 10), 240u);
    capsule(canvas, gx - 0.16f * s, cy + 0.12f * s, gx - 0.12f * s,
            cy + 0.24f * s, 0.024f * s, RGB(92, 40, 10), 240u);
    disc(canvas, gx + 0.22f * s, cy + 0.14f * s, 0.06f * s, RGB(120, 56, 12),
         230u);
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

static void draw_plant_kind(h2_garden_guard_canvas_t* canvas, uint8_t kind,
                            float gx, float gy, float s, uint32_t stage) {
  switch (kind) {
    case H2_GARDEN_GUARD_PLANT_SUNFLOWER:
      draw_sunflower(canvas, gx, gy, s, stage);
      break;
    case H2_GARDEN_GUARD_PLANT_SHOOTER:
      draw_shooter(canvas, gx, gy, s, stage);
      break;
    case H2_GARDEN_GUARD_PLANT_PUMPKIN:
      draw_pumpkin(canvas, gx, gy, s, stage);
      break;
    default:
      break;
  }
}

/* Side view of a beetle walking left; t is its animation clock in seconds. */
static void draw_beetle(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                        float s, float t, bool eating, bool shellback,
                        bool armored, bool flash) {
  const uint32_t glow = flash ? 140u : 0u;
  const float line = rim_width(s);
  const float bob = eating ? 0.025f * s * sinf(t * 16.0f)
                           : 0.02f * s * fabsf(sinf(t * 11.0f));
  const float nibble = eating ? 0.03f * s * sinf(t * 16.0f) : 0.0f;
  const uint16_t leg_far = lighten(RGB(26, 18, 30), glow);
  const uint16_t leg = lighten(RGB(44, 30, 50), glow);
  ground_shadow(canvas, gx, gy, 0.36f * s);
  static const float hips[3] = {-0.14f, 0.02f, 0.18f};
  for (unsigned k = 0; k < 3u; ++k) {
    const float swing =
        eating ? 0.0f : 0.06f * s * sinf(t * 11.0f + (float)k * 2.1f);
    const float hx = gx + hips[k] * s + 0.04f * s;
    capsule(canvas, hx, gy - 0.2f * s - bob, hx + 0.05f * s - swing,
            gy - 0.02f * s, 0.022f * s, leg_far, 256u);
  }
  const float by = gy - 0.3f * s - bob;
  const uint16_t shell =
      lighten(shellback ? RGB(64, 120, 70) : RGB(112, 70, 164), glow);
  oellipse(canvas, gx + 0.06f * s, by, 0.32f * s, 0.21f * s, line, shell,
           darken(shell, 150u));
  ellipse(canvas, gx + 0.02f * s, by - 0.09f * s, 0.2f * s, 0.06f * s,
          lighten(shell, 120u), 120u);
  if (!shellback) {
    disc(canvas, gx + 0.14f * s, by - 0.01f * s, 0.045f * s, darken(shell, 90u),
         256u);
    disc(canvas, gx - 0.03f * s, by + 0.05f * s, 0.035f * s, darken(shell, 90u),
         256u);
    disc(canvas, gx + 0.26f * s, by + 0.06f * s, 0.03f * s, darken(shell, 90u),
         256u);
  }
  if (armored) {
    const uint16_t steel = lighten(RGB(164, 172, 186), glow);
    oellipse(canvas, gx + 0.05f * s, by - 0.05f * s, 0.3f * s, 0.17f * s, line,
             steel, RGB(70, 76, 90));
    capsule(canvas, gx - 0.22f * s, by + 0.07f * s, gx + 0.32f * s,
            by + 0.07f * s, 0.03f * s, darken(steel, 70u), 256u);
    ellipse(canvas, gx - 0.02f * s, by - 0.12f * s, 0.16f * s, 0.04f * s,
            RGB(240, 244, 250), 150u);
    for (int k = -1; k <= 1; ++k)
      disc(canvas, gx + 0.05f * s + (float)k * 0.13f * s, by + 0.07f * s,
           0.022f * s, darken(steel, 130u), 256u);
  }
  for (unsigned k = 0; k < 3u; ++k) {
    const float swing =
        eating ? 0.0f : 0.06f * s * sinf(t * 11.0f + (float)k * 2.1f + 3.1f);
    const float hx = gx + hips[k] * s;
    const float kx = hx - 0.05f * s - swing * 0.5f, ky = gy - 0.11f * s;
    capsule(canvas, hx, gy - 0.18f * s - bob, kx, ky, 0.026f * s, leg, 256u);
    capsule(canvas, kx, ky, hx - 0.03f * s - swing, gy, 0.024f * s, leg, 256u);
  }
  const float hx = gx - 0.26f * s - nibble, hy = gy - 0.27f * s - bob * 0.5f;
  const uint16_t head = lighten(RGB(52, 36, 60), glow);
  capsule(canvas, hx + 0.02f * s, hy - 0.11f * s, hx - 0.06f * s,
          hy - 0.28f * s, 0.016f * s, leg_far, 256u);
  odisc(canvas, hx, hy, 0.13f * s, line, head, RGB(20, 12, 24));
  capsule(canvas, hx - 0.03f * s, hy - 0.11f * s, hx - 0.13f * s,
          hy - 0.26f * s, 0.016f * s, leg, 256u);
  disc(canvas, hx - 0.13f * s, hy - 0.27f * s, 0.03f * s, leg, 256u);
  odisc(canvas, hx - 0.04f * s, hy - 0.04f * s, 0.05f * s, line * 0.6f,
        RGB(252, 248, 226), RGB(20, 12, 24));
  disc(canvas, hx - 0.06f * s, hy - 0.04f * s, 0.025f * s, RGB(16, 12, 12),
       256u);
  if (eating)
    capsule(canvas, hx - 0.11f * s, hy + 0.05f * s, hx - 0.17f * s,
            hy + 0.02f * s + nibble, 0.02f * s, RGB(30, 20, 20), 256u);
}

/* Side view of a running ant heading left. */
static void draw_runner(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                        float s, float t, bool eating, bool flash) {
  const uint32_t glow = flash ? 140u : 0u;
  const float line = rim_width(s);
  const float bob =
      eating ? 0.02f * s * sinf(t * 18.0f) : 0.03f * s * fabsf(sinf(t * 22.0f));
  const uint16_t leg_far = lighten(RGB(70, 22, 14), glow);
  const uint16_t leg = lighten(RGB(96, 30, 18), glow);
  const uint16_t body = lighten(RGB(200, 64, 40), glow);
  const uint16_t rim = RGB(80, 18, 10);
  ground_shadow(canvas, gx, gy, 0.3f * s);
  for (int k = -1; k <= 1; ++k) {
    const float swing =
        eating ? 0.0f : 0.08f * s * sinf(t * 22.0f + (float)k * 2.1f);
    const float hx = gx + (float)k * 0.04f * s + 0.03f * s;
    capsule(canvas, hx, gy - 0.26f * s - bob,
            hx + (float)k * 0.12f * s + swing * 0.4f, gy - 0.34f * s - bob,
            0.016f * s, leg_far, 256u);
    capsule(canvas, hx + (float)k * 0.12f * s + swing * 0.4f,
            gy - 0.34f * s - bob, hx + (float)k * 0.22f * s + swing,
            gy - 0.01f * s, 0.016f * s, leg_far, 256u);
  }
  oellipse(canvas, gx + 0.2f * s, gy - 0.32f * s - bob, 0.17f * s, 0.13f * s,
           line, body, rim);
  ellipse(canvas, gx + 0.16f * s, gy - 0.38f * s - bob, 0.08f * s, 0.04f * s,
          lighten(body, 120u), 140u);
  oellipse(canvas, gx, gy - 0.28f * s - bob, 0.09f * s, 0.07f * s, line, body,
           rim);
  for (int k = -1; k <= 1; ++k) {
    const float swing =
        eating ? 0.0f : 0.08f * s * sinf(t * 22.0f + (float)k * 2.1f + 3.1f);
    const float hx = gx + (float)k * 0.04f * s;
    const float kx = hx + (float)k * 0.13f * s - swing * 0.4f;
    const float ky = gy - 0.36f * s - bob;
    capsule(canvas, hx, gy - 0.25f * s - bob, kx, ky, 0.02f * s, leg, 256u);
    capsule(canvas, kx, ky, hx + (float)k * 0.24f * s - swing, gy, 0.019f * s,
            leg, 256u);
  }
  const float hx = gx - 0.17f * s, hy = gy - 0.33f * s - bob;
  odisc(canvas, hx, hy, 0.1f * s, line, body, rim);
  capsule(canvas, hx - 0.02f * s, hy - 0.08f * s, hx - 0.1f * s, hy - 0.2f * s,
          0.014f * s, leg, 256u);
  capsule(canvas, hx - 0.1f * s, hy - 0.2f * s, hx - 0.17f * s, hy - 0.18f * s,
          0.014f * s, leg, 256u);
  disc(canvas, hx - 0.04f * s, hy - 0.03f * s, 0.03f * s, RGB(20, 10, 8), 256u);
  disc(canvas, hx - 0.035f * s, hy - 0.04f * s, 0.01f * s, RGB(255, 240, 220),
       256u);
  capsule(canvas, hx - 0.08f * s, hy + 0.05f * s, hx - 0.14f * s,
          hy + (eating ? 0.03f + 0.03f * sinf(t * 18.0f) : 0.06f) * s,
          0.018f * s, RGB(50, 16, 10), 256u);
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
  return scale565(RGB(98, 64, 38), k);
}

/* Front face of the raised lawn bed below its near edge. */
static uint16_t facade_color(const h2_garden_guard_layout_t* l, float x,
                             float y, uint32_t noise) {
  const float depth = y - l->near_y;
  const float height = (float)l->height - l->near_y;
  const float lawn_x0 = l->mid_x - 0.5f * l->near_w;
  const float lawn_x1 = l->mid_x + 0.5f * l->near_w;
  const float lip = fmaxf(2.0f, 3.0f * l->unit);
  if (depth < lip)
    return x >= lawn_x0 && x < lawn_x1 ? RGB(62, 118, 42) : RGB(214, 206, 188);
  const float course = fmaxf((height - lip) * 0.5f, 4.0f);
  const float block = fmaxf(l->near_w * 0.075f, 8.0f);
  const int32_t row = (int32_t)((depth - lip) / course);
  const float shifted = x + ((row & 1) ? block * 0.5f : 0.0f);
  const int32_t brick = (int32_t)floorf(shifted / block);
  const uint32_t tone =
      hash32((uint32_t)row * 2654435761u ^ (uint32_t)brick * 97u) & 31u;
  uint16_t color = RGB(170u + tone / 2u, 150u + tone / 2u, 118u + tone / 3u);
  if (fmodf(depth - lip, course) < 1.2f ||
      fractf(shifted / block) * block < 1.2f)
    color = RGB(104, 88, 66);
  else if ((noise & 31u) < 2u)
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
    uint16_t c = RGB(176, 84, 56);
    if (seam_px(tiles, 0.025f * px_per_unit) < 0.7f) c = RGB(120, 52, 36);
    if (hgt < WALL_HEIGHT + 0.012f) c = RGB(96, 44, 30);
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
      *out = RGB(246, 244, 236);
    else
      *out = fractf((local + hv) * 1.3f) < 0.18f ? RGB(200, 228, 244)
                                                 : RGB(112, 160, 196);
    return true;
  }
  uint16_t c = RGB(214, 196, 160);
  if (seam_px(hgt / 0.024f, 0.024f * px_per_unit) < 0.6f)
    c = RGB(176, 158, 124);
  *out = c;
  return true;
}

static uint16_t lawn_color(uint32_t lane, uint32_t col, float f, float seam,
                           uint32_t noise) {
  const bool odd = ((lane + col) & 1u) != 0u;
  uint16_t color = odd ? RGB(98, 168, 60) : RGB(122, 190, 74);
  float k =
      0.97f + 0.05f * (float)(hash32(lane * 31u + col * 7u + 3u) & 15u) / 15.0f;
  /* Farther grass is a little darker and cooler. */
  k *= 1.0f - 0.14f * clampf(f, 0.0f, 1.0f);
  const uint32_t speck = noise & 63u;
  if (speck < 3u)
    k *= 0.84f;
  else if (speck < 5u)
    k *= 1.12f;
  if (seam < 0.8f) k *= 0.86f;
  return scale565(color, k);
}

static uint16_t sky_color(float y, float top, float horizon) {
  const float s = clampf((y - top) / fmaxf(horizon - top, 1.0f), 0.0f, 1.0f);
  return blend(RGB(118, 184, 234), RGB(214, 236, 242), (uint32_t)(s * 256.0f));
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
    /* A few clouds high in the sky. */
    const float sky = hedge_low - l->bar_h;
    if (sky > 60.0f) {
      static const float clouds[][2] = {
          {0.2f, 0.22f}, {0.72f, 0.12f}, {0.5f, 0.42f}};
      for (unsigned i = 0; i < sizeof(clouds) / sizeof(clouds[0]); ++i) {
        const float x = (float)l->width * clouds[i][0];
        const float y = l->bar_h + sky * clouds[i][1] + 10.0f * l->unit;
        const float r = 16.0f * l->unit;
        disc(canvas, x - r, y + r * 0.2f, r * 0.8f, RGB(250, 252, 255), 220u);
        disc(canvas, x + r, y + r * 0.25f, r * 0.75f, RGB(250, 252, 255), 220u);
        disc(canvas, x, y, r, RGB(255, 255, 255), 230u);
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
  static const uint16_t blooms[4] = {RGB(244, 120, 160), RGB(255, 246, 236),
                                     RGB(255, 210, 60), RGB(170, 120, 230)};
  for (unsigned i = 0; i < 34u; ++i) {
    const uint32_t h = hash32(i * 2654435761u + 11u);
    const float u =
        -0.15f + 1.3f * ((float)i + (float)(h & 255u) / 512.0f) / 34.0f;
    const float f =
        1.035f + (BED_DEPTH - 0.07f) * (float)((h >> 8) & 255u) / 255.0f;
    float x, y;
    h2_garden_guard_ground_point(l, u, f, &x, &y);
    const float r =
        fmaxf(0.0085f * l->near_w * h2_garden_guard_depth_scale(l, f), 1.2f);
    capsule(canvas, x, y, x, y - r * 2.2f, r * 0.3f, RGB(56, 116, 46), 256u);
    const uint16_t c = blooms[(h >> 16) & 3u];
    for (unsigned k = 0; k < 5u; ++k) {
      const float a = (float)k * (2.0f * H2_GARDEN_GUARD_PI / 5.0f);
      disc(canvas, x + cosf(a) * r * 0.7f, y - r * 2.4f + sinf(a) * r * 0.7f,
           r * 0.55f, c, 256u);
    }
    disc(canvas, x, y - r * 2.4f, r * 0.4f, RGB(250, 200, 40), 256u);
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
      uint16_t color = RGB(104, 72, 48);
      if (fy < l->bar_h) {
        /* Toolbar plank with a dark lower edge. */
        color = wood_color(fx, fy, plank, noise);
        if (fy > l->bar_h - 3.0f || fmodf(fy, plank) < 1.0f)
          color = RGB(54, 34, 20);
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
              RGB(184, 138, 90),
              0.92f +
                  0.12f *
                      (float)(hash32((uint32_t)(int32_t)floorf(board) + 77u) &
                              15u) /
                      15.0f);
          if (seam_px(board, 0.03f * fence_px) < 0.8f) color = RGB(110, 76, 46);
          if (hgt > FENCE_HEIGHT - 0.01f) color = RGB(206, 164, 112);
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
                      ? RGB(206, 200, 186)
                      : scale565(RGB(104, 72, 48),
                                 0.85f + 0.3f * (float)(noise & 15u) / 15.0f);
        } else if (u >= 0.0f && u < 1.0f) {
          const uint32_t lane = (uint32_t)clampf((1.0f - f) * (float)LANES,
                                                 0.0f, (float)LANES - 1.0f);
          const uint32_t col =
              (uint32_t)clampf(u * (float)COLS, 0.0f, (float)COLS - 1.0f);
          const float seam = fminf(seam_px(u * (float)COLS, col_px),
                                   seam_px((1.0f - f) * (float)LANES, lane_px));
          color = lawn_color(lane, col, f, seam, noise);
        } else if (u < 0.0f && u >= -CURB) {
          color = RGB(214, 208, 194);
        } else if (u > 1.0f && u <= 1.0f + CURB) {
          color = RGB(214, 208, 194);
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
          /* Gravel path where pests come in. */
          const uint32_t speck = noise & 31u;
          color = scale565(RGB(172, 146, 108),
                           speck < 4u ? 0.78f : (speck < 7u ? 1.12f : 1.0f));
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
    out->values[6] = game->wave;
    out->values[7] = progress_step(game);
    return;
  }
  uint32_t carts = 0;
  for (unsigned lane = 0; lane < LANES; ++lane)
    if (game->carts[lane].state == H2_GARDEN_GUARD_CART_READY)
      carts |= 1u << lane;
  out->values[0] = game->plant_revision;
  out->values[1] = carts;
  out->values[2] = game->score;
  out->values[3] = game->best;
  out->values[4] = game->new_best ? 1u : 0u;
  out->values[5] = prompt_visible(game) ? 1u : 0u;
  out->values[6] = game->kills;
}

static void draw_card(h2_garden_guard_canvas_t* canvas,
                      const h2_garden_guard_game_t* game, unsigned card) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float x0 = l->card_x[card], y0 = l->card_y;
  const float x1 = x0 + l->card_w, y1 = y0 + l->card_h;
  const float radius = l->card_w * 0.14f;
  const uint32_t cost = h2_garden_guard_card_cost((h2_garden_guard_card_t)card);
  const bool affordable = game->sun >= cost;
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
               affordable ? RGB(132, 196, 228) : RGB(120, 128, 136), 256u);
  /* A strip of lawn at the bottom of the well for the figure to stand on. */
  const float well_h = well_y1 - y0 - inset;
  rounded_rect(canvas, x0 + inset, well_y1 - well_h * 0.24f, x1 - inset,
               well_y1, radius * 0.5f,
               affordable ? RGB(104, 172, 64) : RGB(104, 112, 98), 256u);
  const float cx = (x0 + x1) * 0.5f;
  if (card == H2_GARDEN_GUARD_CARD_SHOVEL) {
    draw_shovel(canvas, cx, (y0 + inset + well_y1) * 0.5f,
                fminf(l->card_w - 2.0f * inset, well_h));
  } else {
    const uint8_t kind = (uint8_t)(card + 1u);
    const float height =
        h2_garden_guard_plant_height((h2_garden_guard_plant_kind_t)kind);
    const float s =
        fminf(well_h * 0.84f / height, (l->card_w - 2.0f * inset) * 1.25f);
    const float gx =
        kind == H2_GARDEN_GUARD_PLANT_SHOOTER ? cx - 0.06f * s : cx;
    draw_plant_kind(canvas, kind, gx, well_y1 - well_h * 0.1f, s, 0u);
  }
  if (cost > 0u) {
    char value[12];
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
  if (game->card_denied_ms[card] != 0u)
    rounded_rect(canvas, x0, y0, x1, y1, radius, RGB(230, 40, 32), 120u);
}

static void draw_toolbar(h2_garden_guard_canvas_t* canvas,
                         const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float u = l->unit;
  rounded_rect(canvas, l->sun_x, l->card_y, l->sun_x + l->sun_w,
               l->card_y + l->card_h, l->card_w * 0.14f, RGB(40, 24, 12), 170u);
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
  rounded_rect(canvas, x0, bar_y0, x1, bar_y1, radius, RGB(30, 18, 10), 220u);
  const uint32_t step = progress_step(game);
  if (step > 0u) {
    const float inset = fmaxf(1.0f, 2.0f * u);
    const float fill =
        (x1 - x0 - 2.0f * inset) * (float)step / (float)PROGRESS_STEPS;
    rounded_rect(canvas, x0 + inset, bar_y0 + inset, x0 + inset + fill,
                 bar_y1 - inset, fmaxf(radius - inset, 0.0f), RGB(132, 206, 72),
                 256u);
  }
}

/* Dark glass panel centred in the area below the toolbar. */
static void panel(h2_garden_guard_canvas_t* canvas,
                  const h2_garden_guard_layout_t* l, float width_fraction,
                  float height_fraction, float* out_x0, float* out_y0,
                  float* out_w, float* out_h) {
  const float area_h = (float)l->height - l->bar_h;
  const float w = fminf((float)l->width * width_fraction, 560.0f * l->unit);
  const float h = fminf(area_h * height_fraction, 400.0f * l->unit);
  const float x0 = ((float)l->width - w) * 0.5f;
  const float y0 = l->bar_h + (area_h - h) * 0.5f;
  rounded_rect(canvas, x0, y0, x0 + w, y0 + h, 18.0f * l->unit, RGB(22, 34, 18),
               205u);
  *out_x0 = x0;
  *out_y0 = y0;
  *out_w = w;
  *out_h = h;
}

static void draw_title(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float u = l->unit;
  float x0, y0, w, h;
  panel(canvas, l, 0.86f, 0.86f, &x0, &y0, &w, &h);
  const float cx = x0 + w * 0.5f;
  const int32_t big = fit_scale("GARDEN GUARD", w * 0.9f, text_scale(u, 6.0f));
  text_centered(canvas, "GARDEN GUARD", cx, y0 + h * 0.06f, big,
                RGB(255, 214, 60), 256u);
  /* The cast stands on a strip of lawn. */
  const float s = fminf(w / 5.0f, h * 0.3f);
  const float ground = y0 + h * 0.56f;
  rounded_rect(canvas, x0 + w * 0.06f, ground - 0.1f * s, x0 + w * 0.94f,
               ground + 0.12f * s, 0.11f * s, RGB(92, 156, 58), 230u);
  draw_sunflower(canvas, x0 + w * 0.17f, ground, s, 0u);
  draw_shooter(canvas, x0 + w * 0.37f, ground, s, 0u);
  draw_pumpkin(canvas, x0 + w * 0.6f, ground, s, 0u);
  draw_beetle(canvas, x0 + w * 0.83f, ground, s * 0.95f, 0.0f, false, false,
              false, false);
  const char* hint1 = "TAP SUN TO COLLECT IT";
  const char* hint2 = "PICK A CARD, THEN A TILE";
  const int32_t hint = fit_scale(hint2, w * 0.92f, text_scale(u, 2.6f));
  text_centered(canvas, hint1, cx, y0 + h * 0.63f, hint, RGB(232, 240, 214),
                230u);
  text_centered(canvas, hint2, cx, y0 + h * 0.63f + (float)(hint * 10), hint,
                RGB(232, 240, 214), 230u);
  const int32_t start =
      fit_scale("TAP TO START", w * 0.9f, text_scale(u, 3.0f));
  /* Keep a gap below the hints when the panel is short. */
  const float start_y =
      fmaxf(y0 + h * 0.8f, y0 + h * 0.63f + (float)(hint * 20) + 6.0f * u);
  text_centered(canvas, "TAP TO START", cx, start_y, start, RGB(255, 244, 200),
                256u);
  if (game->best > 0u) {
    char value[24];
    (void)snprintf(value, sizeof(value), "BEST %u", (unsigned)game->best);
    text_centered(canvas, value, cx, start_y + (float)(start * 10), hint,
                  RGB(255, 222, 80), 230u);
  }
}

static void draw_result(h2_garden_guard_canvas_t* canvas,
                        const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float u = l->unit;
  float x0, y0, w, h;
  panel(canvas, l, 0.82f, 0.76f, &x0, &y0, &w, &h);
  const float cx = x0 + w * 0.5f;
  const bool won = game->phase == H2_GARDEN_GUARD_PHASE_WON;
  const char* title = won ? "GARDEN SAFE!" : "BUGS GOT IN!";
  const int32_t big = fit_scale(title, w * 0.9f, text_scale(u, 5.0f));
  text_centered(canvas, title, cx, y0 + h * 0.08f, big,
                won ? RGB(170, 236, 90) : RGB(255, 96, 72), 256u);
  char value[32];
  (void)snprintf(value, sizeof(value), "SCORE %u", (unsigned)game->score);
  const int32_t middle = fit_scale(value, w * 0.9f, text_scale(u, 3.0f));
  float y = y0 + h * 0.08f + (float)(big * 11);
  text_centered(canvas, value, cx, y, middle, RGB(255, 236, 200), 256u);
  y += (float)(middle * 11);
  (void)snprintf(value, sizeof(value), "BUGS STOPPED %u",
                 (unsigned)game->kills);
  const int32_t small = fit_scale(value, w * 0.9f, text_scale(u, 2.5f));
  text_centered(canvas, value, cx, y, small, RGB(220, 230, 200), 230u);
  y += (float)(small * 11);
  if (game->new_best)
    (void)snprintf(value, sizeof(value), "NEW BEST!");
  else
    (void)snprintf(value, sizeof(value), "BEST %u", (unsigned)game->best);
  text_centered(canvas, value, cx, y, small,
                game->new_best ? RGB(255, 222, 80) : RGB(220, 214, 200), 240u);
  if (prompt_visible(game)) {
    const int32_t prompt =
        fit_scale("TAP TO PLAY AGAIN", w * 0.9f, text_scale(u, 2.5f));
    text_centered(canvas, "TAP TO PLAY AGAIN", cx,
                  y0 + h - (float)(prompt * 7) - 14.0f * u, prompt,
                  RGB(255, 244, 200), 256u);
  }
}

/* Plants and parked carts, far lane first so nearer figures overlap; each
 * claims its pixels in the occlusion map. */
static void draw_field(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
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
    for (uint32_t col = 0; col < COLS; ++col) {
      const h2_garden_guard_plant_t* plant = &game->plants[lane][col];
      if (plant->kind == H2_GARDEN_GUARD_PLANT_NONE) continue;
      float x, y;
      h2_garden_guard_field_point(l, lane, (float)col + 0.5f, &x, &y);
      draw_plant_kind(canvas, plant->kind, x, y, s, plant->stage);
    }
  }
  depth_mode(canvas, DEPTH_OFF, 0u);
  full_clip(canvas);
  if (game->phase == H2_GARDEN_GUARD_PHASE_TITLE)
    draw_title(canvas, game);
  else if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING)
    draw_result(canvas, game);
}

void h2_garden_guard_render_static(h2_garden_guard_canvas_t* canvas,
                                   const h2_garden_guard_game_t* game,
                                   h2_garden_guard_part_t part) {
  full_clip(canvas);
  depth_mode(canvas, DEPTH_OFF, 0u);
  if (part == H2_GARDEN_GUARD_PART_TOOLBAR)
    draw_toolbar(canvas, game);
  else
    draw_field(canvas, game);
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
  field_clip(canvas, l);
  draw_hover(canvas, game);
  /* Lane by lane, far to near; nearer plants hide what stands behind them. */
  for (uint32_t lane = 0; lane < LANES; ++lane) {
    depth_mode(canvas, DEPTH_TEST, lane);
    const float s = h2_garden_guard_lane_size(l, lane);
    const h2_garden_guard_cart_t* cart = &game->carts[lane];
    if (cart->state == H2_GARDEN_GUARD_CART_SWEEPING) {
      float x, y;
      h2_garden_guard_field_point(l, lane, cart->x, &x, &y);
      draw_cart(canvas, x, y, s * 0.62f);
    }
    for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i) {
      const h2_garden_guard_pest_t* pest = &game->pests[i];
      if (!pest->active || pest->lane != lane) continue;
      float x, y;
      h2_garden_guard_field_point(l, lane, pest->x, &x, &y);
      const float t =
          (float)((game->time_ms + pest->phase_ms) % 600000u) * 0.001f;
      const bool flash = pest->flash_ms != 0u;
      if (pest->kind == H2_GARDEN_GUARD_PEST_RUNNER) {
        draw_runner(canvas, x, y, s * 0.92f, t, pest->eating, flash);
      } else {
        const bool shellback = pest->kind == H2_GARDEN_GUARD_PEST_SHELLBACK;
        draw_beetle(canvas, x, y, s * (shellback ? 1.04f : 0.98f), t,
                    pest->eating, shellback, shellback && pest->armor > 0,
                    flash);
      }
    }
    for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PEAS; ++i) {
      const h2_garden_guard_pea_t* pea = &game->peas[i];
      if (!pea->active || pea->lane != lane) continue;
      float x, y;
      h2_garden_guard_field_point(l, lane, pea->x, &x, &y);
      const float r = s * 0.085f;
      ellipse(canvas, x, y, r * 0.9f, r * 0.3f, RGB(18, 40, 14), 80u);
      const float py = y - H2_GARDEN_GUARD_PEA_HEIGHT * s;
      odisc(canvas, x, py, r, rim_width(s), RGB(140, 222, 84),
            RGB(40, 104, 30));
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
