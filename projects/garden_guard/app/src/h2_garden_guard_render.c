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

static void draw_sunflower(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                           float s, uint32_t stage,
                           h2_garden_guard_plant_pose_t pose) {
  s *= pose.growth;
  const uint32_t dim = stage * 46u;
  const float line = rim_width(s);
  const uint16_t rim = RGB(34, 72, 26);
  ground_shadow(canvas, gx, gy, 0.3f * s);
  const float hx = gx + pose.sway * s;
  const float hy = gy - (0.74f + pose.lift) * s;
  ocapsule(canvas, gx, gy - 0.02f * s, hx, hy + 0.10f * s, 0.045f * s, line,
           darken(RGB(80, 154, 52), dim), rim);
  oellipse(canvas, gx - 0.15f * s, gy - (0.22f + pose.sway) * s, 0.15f * s,
           0.065f * s, line, darken(RGB(92, 176, 62), dim), rim);
  oellipse(canvas, gx + 0.15f * s, gy - (0.35f - pose.sway + pose.lift) * s,
           0.14f * s, 0.06f * s, line, darken(RGB(92, 176, 62), dim), rim);
  // A crown of overlapping petals. Keep the inner highlight on the lit side.
  static const float ring[][2] = {
      {1, 0},           {0.866f, 0.5f},  {0.5f, 0.866f},  {0, 1},
      {-0.5f, 0.866f},  {-0.866f, 0.5f}, {-1, 0},         {-0.866f, -0.5f},
      {-0.5f, -0.866f}, {0, -1},         {0.5f, -0.866f}, {0.866f, -0.5f},
  };
  const float spread = (0.218f + 0.04f * pose.action) * s;
  for (unsigned k = 0; k < 12u; ++k)
    disc(canvas, hx + ring[k][0] * spread, hy + ring[k][1] * spread,
         0.105f * s + line, RGB(139, 75, 18), 256u);
  for (unsigned k = 0; k < 12u; ++k) {
    const uint16_t petal =
        ring[k][1] < 0.1f ? RGB(255, 210, 74) : RGB(243, 163, 37);
    disc(canvas, hx + ring[k][0] * spread, hy + ring[k][1] * spread, 0.105f * s,
         lighten(darken(petal, dim), (uint32_t)(100.0f * pose.action)), 256u);
  }
  disc(canvas, hx, hy, 0.206f * s, RGB(255, 222, 100), 256u);
  odisc(canvas, hx, hy, 0.15f * s, line, darken(RGB(128, 78, 34), dim),
        RGB(70, 40, 14));
  disc(canvas, hx, hy + 0.01f * s, 0.11f * s, darken(RGB(152, 96, 42), dim),
       256u);
  const uint16_t ink = RGB(36, 20, 8);
  for (int side = -1; side <= 1; side += 2) {
    const float eye = hx + (float)side * 0.055f * s;
    ellipse(canvas, eye, hy - 0.03f * s, 0.03f * s,
            (pose.blink ? 0.012f : 0.035f) * s, ink, 256u);
    if (!pose.blink)
      disc(canvas, eye - 0.008f * s, hy - 0.041f * s, 0.012f * s,
           RGB(255, 250, 230), 256u);
  }
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
                         float s, uint32_t stage,
                         h2_garden_guard_plant_pose_t pose) {
  s *= pose.growth;
  const uint32_t dim = stage * 46u;
  const float line = rim_width(s);
  const uint16_t rim = RGB(30, 66, 24);
  const uint16_t leaf = darken(RGB(76, 156, 54), dim);
  ground_shadow(canvas, gx, gy, 0.28f * s);
  ocapsule(canvas, gx, gy - 0.03f * s, gx - 0.24f * s,
           gy - (0.1f + 1.4f * pose.sway) * s, 0.055f * s, line, leaf, rim);
  ocapsule(canvas, gx, gy - 0.03f * s, gx + 0.22f * s,
           gy - (0.08f - pose.sway) * s, 0.05f * s, line, leaf, rim);
  const float recoil = pose.action * pose.action;
  const float hx = gx + (-0.03f + pose.sway - recoil * 0.19f) * s;
  const float hy = gy - (0.62f + 0.06f * recoil) * s;
  ocapsule(canvas, gx, gy - 0.04f * s, hx, hy + 0.17f * s, 0.055f * s, line,
           darken(RGB(84, 160, 54), dim), rim);

  // The seed pod opens into a flared copper-green trumpet, not a round snout.
  const uint16_t pod = darken(RGB(104, 191, 122), dim);
  oellipse(canvas, hx - 0.056f * s, hy + 0.018f * s, 0.215f * s, 0.246f * s,
           line, pod, rim);
  ellipse(canvas, hx - 0.13f * s, hy - 0.055f * s, 0.089f * s, 0.15f * s,
          RGB(198, 231, 140), 160u);
  ocapsule(canvas, hx - 0.08f * s, hy - 0.18f * s, hx - 0.14f * s,
           hy - (0.34f - 1.5f * pose.sway + 0.05f * recoil) * s, 0.043f * s,
           line, leaf, rim);
  ocapsule(canvas, hx - 0.06f * s, hy - 0.22f * s, hx + 0.045f * s,
           hy - (0.34f + 0.9f * pose.sway) * s, 0.034f * s, line,
           RGB(141, 206, 114), rim);
  ocapsule(canvas, hx + 0.09f * s, hy + 0.025f * s, hx + 0.28f * s,
           hy - 0.005f * s, 0.088f * s, line, RGB(153, 184, 88), rim);
  oellipse(canvas, hx + 0.32f * s, hy, 0.094f * s,
           (0.126f + 0.022f * recoil) * s, line, RGB(213, 211, 109), rim);
  ellipse(canvas, hx + 0.34f * s, hy, 0.048f * s, 0.082f * s,
          recoil > 0.58f ? RGB(250, 245, 161) : RGB(41, 80, 58), 256u);
  const float eye = hx - 0.045f * s;
  if (pose.blink) {
    capsule(canvas, eye - 0.049f * s, hy - 0.055f * s, eye + 0.047f * s,
            hy - 0.055f * s, 0.018f * s, rim, 256u);
  } else {
    oellipse(canvas, eye, hy - 0.065f * s, 0.053f * s, 0.071f * s, line,
             RGB(245, 240, 188), rim);
    disc(canvas, eye + 0.016f * s, hy - 0.065f * s, 0.028f * s, RGB(26, 51, 40),
         256u);
  }
}

static void draw_pumpkin(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                         float s, uint32_t stage,
                         h2_garden_guard_plant_pose_t pose) {
  s *= pose.growth;
  const uint32_t dim = stage * 30u;
  const float squash = 1.0f - 0.11f * pose.impact;
  gx -= 0.075f * pose.impact * s;
  const float line = rim_width(s);
  const uint16_t rim = RGB(120, 52, 10);
  const float cy = gy - (0.3f * squash + pose.lift) * s;
  const uint32_t flash = (uint32_t)(128.0f * pose.impact);
  ground_shadow(canvas, gx, gy, 0.42f * s);
  oellipse(canvas, gx - 0.17f * s, cy, 0.21f * s, 0.27f * s * squash, line,
           lighten(darken(RGB(216, 110, 26), dim), flash), rim);
  oellipse(canvas, gx + 0.17f * s, cy, 0.21f * s, 0.27f * s * squash, line,
           lighten(darken(RGB(216, 110, 26), dim), flash), rim);
  oellipse(canvas, gx, cy - 0.01f * s, 0.23f * s, 0.3f * s * squash, line,
           lighten(darken(RGB(242, 138, 36), dim), flash), rim);
  ellipse(canvas, gx - 0.07f * s, cy - 0.09f * s, 0.05f * s, 0.15f * s,
          RGB(255, 196, 120), 110u);
  ellipse(canvas, gx - 0.24f * s, cy - 0.05f * s, 0.035f * s, 0.12f * s,
          RGB(255, 180, 100), 80u);
  ocapsule(canvas, gx, cy - 0.27f * s, gx + 0.05f * s, cy - 0.4f * s,
           0.045f * s, line, RGB(112, 78, 36), RGB(60, 40, 16));
  oellipse(canvas, gx - 0.1f * s, cy - 0.32f * s, 0.08f * s, 0.035f * s, line,
           RGB(84, 160, 54), RGB(30, 66, 24));
  const uint16_t ink = RGB(70, 30, 6);
  ellipse(canvas, gx - 0.09f * s, cy - 0.03f * s, 0.04f * s,
          (pose.blink || pose.impact > 0.4f ? 0.015f : 0.055f) * s, ink, 256u);
  ellipse(canvas, gx + 0.09f * s, cy - 0.03f * s, 0.04f * s,
          (pose.blink || pose.impact > 0.4f ? 0.015f : 0.055f) * s, ink, 256u);
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

static const h2_garden_guard_plant_pose_t k_neutral_plant = {.growth = 1.0f};

static void draw_plant_kind(h2_garden_guard_canvas_t* canvas, uint8_t kind,
                            float gx, float gy, float s, uint32_t stage,
                            h2_garden_guard_plant_pose_t pose) {
  switch (kind) {
    case H2_GARDEN_GUARD_PLANT_SUNFLOWER:
      draw_sunflower(canvas, gx, gy, s, stage, pose);
      break;
    case H2_GARDEN_GUARD_PLANT_SHOOTER:
      draw_shooter(canvas, gx, gy, s, stage, pose);
      break;
    case H2_GARDEN_GUARD_PLANT_PUMPKIN:
      draw_pumpkin(canvas, gx, gy, s, stage, pose);
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

static void zombie_leg(const zombie_brush_t* brush, float hip, float offset,
                       float stride, float lift, bool near, bool runner,
                       uint16_t cloth) {
  const float foot = offset + stride;
  const float knee = offset + stride * 0.60f + 0.065f * hip;
  const float knee_y = -0.22f - 0.42f * lift;
  const float shoe_y = -0.066f - lift;
  const uint16_t jeans = near ? cloth : darken(cloth, 70u);
  const float cuff = runner ? 0.055f : 0.075f;
  zombie_limb(brush, offset, -0.43f, knee, knee_y, cuff, jeans);
  zombie_limb(brush, knee, knee_y, foot, shoe_y, cuff * 0.77f, jeans);
  zombie_limb(brush, foot - 0.092f, shoe_y, foot + 0.035f, shoe_y, 0.042f,
              near ? RGB(114, 81, 69) : RGB(62, 63, 69));
  if (runner && near)
    zombie_oval(brush, foot - 0.062f, shoe_y + 0.008f, 0.065f, 0.017f,
                RGB(229, 207, 155), false);
}

/* The same figure can be posed for the title, a card or the field. */
static void draw_zombie(h2_garden_guard_canvas_t* canvas, float gx, float gy,
                        float size, const h2_garden_guard_pest_t* pest) {
  const bool armored = pest->kind == H2_GARDEN_GUARD_PEST_SHELLBACK;
  const bool runner = pest->kind == H2_GARDEN_GUARD_PEST_RUNNER;
  const bool plated = armored && pest->armor > 0;
  const float s = size * (armored ? 1.02f : (runner ? 0.89f : 0.96f));
  const uint32_t period = runner ? 600u : (armored ? 1400u : 1050u);
  const float angle = (float)(pest->stride_ms % period) *
                      (2.0f * H2_GARDEN_GUARD_PI / (float)period);
  const float stride = sinf(angle), step = cosf(angle);
  const float bite = (float)pest->chew_ms / H2_GARDEN_GUARD_PEST_BITE_MS;
  // Anticipation, open jaw, then a faster snap. Damage remains continuous.
  const float open =
      pest->eating
          ? clampf(fminf(bite / 0.32f, (0.49f - bite) / 0.10f), 0.0f, 1.0f)
          : 0.0f;
  const float lunge =
      pest->eating
          ? 0.21f * clampf(1.0f - fabsf(bite - 0.48f) * 3.5f, 0.0f, 1.0f)
          : 0.0f;
  const float hit =
      pest->active ? (float)pest->flash_ms / H2_GARDEN_GUARD_PEST_HIT_MS : 0.0f;
  const float fall =
      pest->exit_ms == 0u
          ? 0.0f
          : 1.0f - (float)pest->exit_ms / H2_GARDEN_GUARD_PEST_EXIT_MS;
  const float tilt = 1.35f * fall * (2.0f - fall);
  const zombie_brush_t brush = {.canvas = canvas,
                                .x = gx + 0.14f * hit * s,
                                .y = gy + 0.05f * fall * s,
                                .size = s * (1.0f - 0.38f * fall * fall),
                                .edge = rim_width(s) * 0.86f,
                                .cos_angle = cosf(tilt),
                                .sin_angle = sinf(tilt),
                                .ink = RGB(35, 39, 47),
                                .flash = (uint32_t)(165.0f * hit * hit)};
  const float reach =
      pest->eating ? 0.075f : (runner ? 0.25f : (armored ? 0.12f : 0.19f));
  const float lift = pest->eating ? 0.0f : (runner ? 0.19f : 0.085f);
  const uint16_t coat = armored
                            ? RGB(70, 108, 111)
                            : (runner ? RGB(179, 77, 50) : RGB(112, 89, 146));
  const uint16_t pants = RGB(74, 91, 115);
  const uint16_t skin = runner ? RGB(160, 196, 132) : RGB(151, 180, 155);
  ground_shadow(canvas, gx + fall * 0.26f * s, gy, s * (0.30f + fall * 0.20f));
  zombie_leg(&brush, -1.0f, 0.05f, stride * reach, fmaxf(0.0f, step) * lift,
             false, runner, pants);
  zombie_leg(&brush, 1.0f, -0.045f, -stride * reach,
             fmaxf(0.0f, -step) * lift * (runner ? 1.0f : 0.35f), true, runner,
             pants);

  const float sx = -(runner ? 0.13f : 0.045f) - lunge * 0.55f + 0.09f * hit;
  const float sy = -0.66f + (pest->eating ? 0.055f : -0.035f * fabsf(step));
  const float hx = sx - 0.045f - lunge + 0.045f * hit;
  const float hy =
      sy - (runner ? 0.23f : 0.29f) + (pest->eating ? 0.045f : 0.0f);
  const float arm = pest->eating ? -0.35f - lunge
                                 : (runner ? -0.07f + stride * 0.2f
                                           : -0.30f + 0.08f * stride);
  const float hand_y = sy + (pest->eating ? -0.012f : 0.10f + 0.055f * stride);
  zombie_limb(&brush, sx + 0.08f, sy, sx + arm * 0.8f, hand_y - 0.05f, 0.058f,
              darken(skin, 65u));
  zombie_limb(&brush, 0.027f, -0.41f, sx, sy, armored ? 0.158f : 0.117f, coat);
  zombie_oval(&brush, sx + 0.047f, sy - 0.015f, 0.067f, 0.082f,
              lighten(coat, 42u), false);
  if (plated) {
    zombie_limb(&brush, sx - 0.07f, sy - 0.01f, sx - 0.06f, sy + 0.13f, 0.113f,
                RGB(136, 163, 163));
    zombie_oval(&brush, sx - 0.105f, sy + 0.018f, 0.027f, 0.028f,
                RGB(243, 206, 127), false);
    zombie_oval(&brush, sx - 0.112f, sy + 0.12f, 0.022f, 0.025f,
                RGB(202, 216, 195), false);
  } else if (runner) {
    zombie_limb(&brush, hx + 0.07f, hy + 0.155f, sx + 0.28f,
                sy - 0.04f - 0.05f * stride, 0.042f, RGB(223, 119, 57));
  } else {
    zombie_limb(&brush, sx - 0.04f, sy - 0.03f, sx - 0.015f, sy + 0.12f, 0.024f,
                RGB(206, 169, 103));
    zombie_oval(&brush, sx + 0.075f, sy + 0.16f, 0.036f, 0.034f,
                RGB(158, 127, 169), false);
  }
  const float elbow =
      sx + (runner ? 0.09f + 0.07f * stride : -0.08f + 0.03f * stride);
  zombie_limb(&brush, sx + 0.10f, sy + 0.01f, elbow, sy + 0.15f,
              armored ? 0.078f : 0.06f, coat);
  zombie_limb(&brush, elbow, sy + 0.15f, sx + arm, hand_y, 0.044f, skin);
  zombie_oval(&brush, sx + arm - 0.04f, hand_y - 0.01f, 0.065f, 0.052f, skin,
              true);

  zombie_oval(&brush, hx + 0.12f, hy + 0.007f, 0.074f, 0.084f,
              darken(skin, 40u), true);
  zombie_oval(&brush, hx, hy, armored ? 0.178f : 0.159f,
              runner ? 0.146f : 0.163f, skin, true);
  zombie_oval(&brush, hx - 0.045f, hy - 0.07f, 0.094f, 0.058f,
              lighten(skin, 55u), false);
  const float jaw = hy + 0.096f + 0.066f * open;
  zombie_oval(&brush, hx - 0.064f, jaw + 0.029f, 0.105f, 0.069f,
              darken(skin, 35u), true);
  zombie_oval(&brush, hx - 0.085f, jaw - 0.019f, 0.086f, 0.034f + 0.035f * open,
              RGB(57, 38, 55), false);
  zombie_oval(&brush, hx - 0.117f, jaw - 0.041f - 0.024f * open, 0.028f, 0.023f,
              RGB(252, 224, 163), false);
  zombie_oval(&brush, hx - 0.079f, hy - 0.025f, 0.068f, 0.055f,
              RGB(251, 235, 176), true);
  zombie_oval(&brush, hx - 0.104f, hy - 0.025f, 0.025f, 0.031f, RGB(37, 41, 51),
              false);
  zombie_oval(&brush, hx + 0.033f, hy + 0.051f, 0.028f, 0.018f,
              RGB(102, 129, 113), false);
  if (armored) {
    zombie_oval(&brush, hx + 0.035f, hy - 0.134f, 0.157f, 0.10f,
                plated ? RGB(142, 168, 166) : RGB(92, 120, 114), true);
    zombie_limb(&brush, hx - 0.133f, hy - 0.138f, hx + 0.164f, hy - 0.112f,
                0.023f, RGB(215, 213, 177));
  } else {
    zombie_limb(&brush, hx + 0.092f, hy - 0.117f,
                hx + (runner ? 0.23f : 0.035f), hy - 0.204f, 0.033f,
                RGB(75, 75, 88));
  }
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
    out->values[6] = game->wave;
    out->values[7] = progress_step(game);
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
    const uint8_t kind = (uint8_t)(card + 1u);
    const float height =
        h2_garden_guard_plant_height((h2_garden_guard_plant_kind_t)kind);
    const float s =
        fminf(well_h * 0.84f / height, (l->card_w - 2.0f * inset) * 1.25f);
    const float gx =
        kind == H2_GARDEN_GUARD_PLANT_SHOOTER ? cx - 0.06f * s : cx;
    draw_plant_kind(canvas, kind, gx, well_y1 - well_h * 0.1f, s, 0u,
                    k_neutral_plant);
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
  draw_sunflower(canvas, x0 + w * 0.17f, ground, s, 0u, k_neutral_plant);
  draw_shooter(canvas, x0 + w * 0.37f, ground, s, 0u, k_neutral_plant);
  draw_pumpkin(canvas, x0 + w * 0.6f, ground, s, 0u, k_neutral_plant);
  h2_garden_guard_render_zombie_portrait(canvas, H2_GARDEN_GUARD_PEST_SHAMBLER,
                                         x0 + w * 0.84f, ground, s * 0.84f);
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
  const char* title = won ? "GARDEN SAFE!" : "ZOMBIES GOT IN!";
  const int32_t big = fit_scale(title, w * 0.9f, text_scale(u, 5.0f));
  text_centered(canvas, title, cx, y0 + h * 0.08f, big,
                won ? RGB(170, 236, 90) : RGB(255, 96, 72), 256u);
  char value[32];
  (void)snprintf(value, sizeof(value), "SCORE %u", (unsigned)game->score);
  const int32_t middle = fit_scale(value, w * 0.9f, text_scale(u, 3.0f));
  float y = y0 + h * 0.08f + (float)(big * 11);
  text_centered(canvas, value, cx, y, middle, RGB(255, 236, 200), 256u);
  y += (float)(middle * 11);
  (void)snprintf(value, sizeof(value), "STOPPED %u", (unsigned)game->kills);
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
    draw_plant_kind(canvas, plant->kind, x, y, scale, plant->stage, pose);
  }
}

/* Cache the parked carts. On result screens the plants rest under the panel. */
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
    if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING)
      draw_lane_plants(canvas, game, lane, s, false);
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
