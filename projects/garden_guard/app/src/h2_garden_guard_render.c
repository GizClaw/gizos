#include "h2_garden_guard_render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BAND_ROWS H2_GARDEN_GUARD_BAND_ROWS
#define RGB H2_GARDEN_GUARD_RGB
#define COOLDOWN_STEPS 20u
#define PROGRESS_STEPS 40u

typedef struct region {
  int32_t x0, y0, x1, y1; /* Half-open, clipped to the canvas. */
} region_t;

/* Local sprite frame: center, size in pixels and the unit vector it faces.
 * Points are given as (a, b) in sizes: a along the facing direction, b to its
 * right-hand side. Facings are axis-aligned, so ellipses stay axis-aligned. */
typedef struct frame {
  float cx, cy, s, fx, fy;
} frame_t;

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

static void fill_rect(h2_garden_guard_canvas_t* canvas, float x0, float y0,
                      float x1, float y1, uint16_t color, uint32_t alpha) {
  if (alpha == 0u) return;
  region_t g;
  if (!region_begin(canvas, x0, y0, x1, y1, &g)) return;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    for (int32_t px = g.x0; px < g.x1; ++px)
      row[px] = blend(row[px], color, alpha);
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
    const float dy = (float)py + 0.5f - cy;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float dx = (float)px + 0.5f - cx;
      const float d2 = dx * dx + dy * dy;
      if (d2 >= outer) continue;
      const float cover = d2 <= inner ? 1.0f : r + 0.5f - sqrtf(d2);
      row[px] = blend(row[px], color, (uint32_t)(cover * (float)alpha));
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
    const float ny = ((float)py + 0.5f - cy) * iry;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float nx = ((float)px + 0.5f - cx) * irx;
      const float d = sqrtf(nx * nx + ny * ny);
      const float cover = clampf((1.0f - d) * edge + 0.5f, 0.0f, 1.0f);
      if (cover > 0.0f)
        row[px] = blend(row[px], color, (uint32_t)(cover * (float)alpha));
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
      row[px] = blend(row[px], color, (uint32_t)(cover * (float)alpha));
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
        row[px] = blend(row[px], color, (uint32_t)(cover * (float)alpha));
    }
  }
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

static void frame_point(const frame_t* f, float a, float b, float* x,
                        float* y) {
  *x = f->cx + (f->fx * a - f->fy * b) * f->s;
  *y = f->cy + (f->fy * a + f->fx * b) * f->s;
}

static void frame_disc(h2_garden_guard_canvas_t* canvas, const frame_t* f,
                       float a, float b, float r, uint16_t color,
                       uint32_t alpha) {
  float x, y;
  frame_point(f, a, b, &x, &y);
  disc(canvas, x, y, r * f->s, color, alpha);
}

static void frame_capsule(h2_garden_guard_canvas_t* canvas, const frame_t* f,
                          float a0, float b0, float a1, float b1, float r,
                          uint16_t color, uint32_t alpha) {
  float x0, y0, x1, y1;
  frame_point(f, a0, b0, &x0, &y0);
  frame_point(f, a1, b1, &x1, &y1);
  capsule(canvas, x0, y0, x1, y1, r * f->s, color, alpha);
}

/* Ellipse with radius ra along the facing direction and rb across it. */
static void frame_ellipse(h2_garden_guard_canvas_t* canvas, const frame_t* f,
                          float a, float b, float ra, float rb, uint16_t color,
                          uint32_t alpha) {
  float x, y;
  frame_point(f, a, b, &x, &y);
  const bool along_x = fabsf(f->fx) >= fabsf(f->fy);
  ellipse(canvas, x, y, (along_x ? ra : rb) * f->s, (along_x ? rb : ra) * f->s,
          color, alpha);
}

/* Plants face the incoming pests; pests face the house. */
static void plant_facing(const h2_garden_guard_layout_t* l, float* fx,
                         float* fy) {
  *fx = l->portrait ? 0.0f : 1.0f;
  *fy = l->portrait ? -1.0f : 0.0f;
}

static void draw_sunflower(h2_garden_guard_canvas_t* canvas, const frame_t* f,
                           uint32_t stage) {
  const uint32_t dim = stage * 50u;
  const uint16_t leaf = darken(RGB(64, 150, 52), dim);
  frame_disc(canvas, f, -0.08f, -0.34f, 0.15f, leaf, 256u);
  frame_disc(canvas, f, -0.08f, 0.34f, 0.15f, leaf, 256u);
  const uint16_t outer = darken(RGB(240, 166, 24), dim);
  const uint16_t inner = darken(RGB(255, 214, 58), dim);
  for (unsigned k = 0; k < 12u; ++k) {
    const float angle = (float)k * (2.0f * H2_GARDEN_GUARD_PI / 12.0f);
    frame_disc(canvas, f, cosf(angle) * 0.3f, sinf(angle) * 0.3f, 0.125f, outer,
               256u);
  }
  for (unsigned k = 0; k < 12u; ++k) {
    const float angle = ((float)k + 0.5f) * (2.0f * H2_GARDEN_GUARD_PI / 12.0f);
    frame_disc(canvas, f, cosf(angle) * 0.26f, sinf(angle) * 0.26f, 0.1f, inner,
               256u);
  }
  frame_disc(canvas, f, 0.0f, 0.0f, 0.2f, darken(RGB(126, 76, 32), dim), 256u);
  frame_disc(canvas, f, 0.0f, 0.0f, 0.14f, darken(RGB(150, 94, 40), dim), 256u);
  float x, y;
  frame_point(f, 0.0f, 0.0f, &x, &y);
  /* The face looks up at the player regardless of the lane direction. */
  disc(canvas, x - 0.065f * f->s, y - 0.03f * f->s, 0.032f * f->s,
       RGB(36, 20, 8), 256u);
  disc(canvas, x + 0.065f * f->s, y - 0.03f * f->s, 0.032f * f->s,
       RGB(36, 20, 8), 256u);
  capsule(canvas, x - 0.06f * f->s, y + 0.07f * f->s, x + 0.06f * f->s,
          y + 0.07f * f->s, 0.02f * f->s, RGB(36, 20, 8), 220u);
}

static void draw_shooter(h2_garden_guard_canvas_t* canvas, const frame_t* f,
                         uint32_t stage) {
  const uint32_t dim = stage * 50u;
  const uint16_t leaf = darken(RGB(52, 132, 46), dim);
  frame_disc(canvas, f, -0.3f, -0.2f, 0.15f, leaf, 256u);
  frame_disc(canvas, f, -0.3f, 0.2f, 0.15f, leaf, 256u);
  frame_disc(canvas, f, -0.36f, 0.0f, 0.13f, leaf, 256u);
  const uint16_t body = darken(RGB(112, 204, 70), dim);
  frame_capsule(canvas, f, 0.04f, 0.0f, 0.34f, 0.0f, 0.13f,
                darken(RGB(96, 184, 60), dim), 256u);
  frame_disc(canvas, f, -0.06f, 0.0f, 0.29f, body, 256u);
  frame_disc(canvas, f, 0.38f, 0.0f, 0.13f, darken(RGB(70, 146, 46), dim),
             256u);
  frame_disc(canvas, f, 0.4f, 0.0f, 0.07f, RGB(28, 60, 22), 256u);
  frame_disc(canvas, f, -0.16f, -0.12f, 0.08f, lighten(body, 110u), 150u);
  frame_disc(canvas, f, 0.06f, -0.12f, 0.065f, RGB(250, 250, 240), 256u);
  frame_disc(canvas, f, 0.06f, 0.12f, 0.065f, RGB(250, 250, 240), 256u);
  frame_disc(canvas, f, 0.09f, -0.12f, 0.035f, RGB(20, 24, 16), 256u);
  frame_disc(canvas, f, 0.09f, 0.12f, 0.035f, RGB(20, 24, 16), 256u);
}

static void draw_pumpkin(h2_garden_guard_canvas_t* canvas, const frame_t* f,
                         uint32_t stage) {
  const uint32_t dim = stage * 34u;
  frame_disc(canvas, f, 0.0f, 0.0f, 0.43f, darken(RGB(200, 92, 18), dim), 256u);
  frame_disc(canvas, f, 0.0f, 0.0f, 0.39f, darken(RGB(238, 130, 30), dim),
             256u);
  for (unsigned k = 0; k < 8u; ++k) {
    const float angle = (float)k * (2.0f * H2_GARDEN_GUARD_PI / 8.0f);
    frame_capsule(canvas, f, cosf(angle) * 0.1f, sinf(angle) * 0.1f,
                  cosf(angle) * 0.38f, sinf(angle) * 0.38f, 0.022f,
                  darken(RGB(196, 92, 20), dim), 200u);
  }
  frame_disc(canvas, f, -0.13f, -0.13f, 0.12f, RGB(255, 176, 80), 110u);
  frame_disc(canvas, f, 0.0f, 0.0f, 0.09f, RGB(104, 72, 30), 256u);
  frame_disc(canvas, f, 0.1f, 0.08f, 0.07f, RGB(76, 150, 50), 256u);
  if (stage >= 1u) {
    frame_capsule(canvas, f, 0.12f, -0.3f, 0.2f, -0.12f, 0.025f,
                  RGB(92, 40, 10), 240u);
    frame_capsule(canvas, f, 0.2f, -0.12f, 0.3f, -0.04f, 0.025f,
                  RGB(92, 40, 10), 240u);
  }
  if (stage >= 2u) {
    frame_capsule(canvas, f, -0.3f, 0.1f, -0.12f, 0.2f, 0.028f, RGB(92, 40, 10),
                  240u);
    frame_capsule(canvas, f, -0.12f, 0.2f, -0.06f, 0.34f, 0.028f,
                  RGB(92, 40, 10), 240u);
    frame_disc(canvas, f, 0.24f, 0.22f, 0.07f, RGB(120, 56, 12), 230u);
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
                            const frame_t* f, uint32_t stage) {
  switch (kind) {
    case H2_GARDEN_GUARD_PLANT_SUNFLOWER:
      draw_sunflower(canvas, f, stage);
      break;
    case H2_GARDEN_GUARD_PLANT_SHOOTER:
      draw_shooter(canvas, f, stage);
      break;
    case H2_GARDEN_GUARD_PLANT_PUMPKIN:
      draw_pumpkin(canvas, f, stage);
      break;
    default:
      break;
  }
}

static void draw_beetle(h2_garden_guard_canvas_t* canvas, const frame_t* f,
                        float t, bool eating, bool shellback, bool armored,
                        bool flash) {
  const uint32_t glow = flash ? 140u : 0u;
  const float wiggle = eating ? 0.0f : sinf(t * 11.0f) * 0.07f;
  const float bob = eating ? sinf(t * 16.0f) * 0.05f : 0.0f;
  const uint16_t legs = lighten(RGB(34, 24, 30), glow);
  for (int k = -1; k <= 1; ++k) {
    for (int side = -1; side <= 1; side += 2) {
      const float a0 = (float)k * 0.17f;
      const float swing = ((k + side) & 2) != 0 ? wiggle : -wiggle;
      frame_capsule(canvas, f, a0, (float)side * 0.14f,
                    a0 + (float)k * 0.08f + swing, (float)side * 0.42f, 0.035f,
                    legs, 256u);
    }
  }
  const uint16_t head = lighten(RGB(44, 32, 52), glow);
  frame_capsule(canvas, f, 0.38f + bob, -0.06f, 0.6f + bob, -0.2f, 0.022f, legs,
                256u);
  frame_capsule(canvas, f, 0.38f + bob, 0.06f, 0.6f + bob, 0.2f, 0.022f, legs,
                256u);
  const uint16_t shell =
      lighten(shellback ? RGB(62, 112, 66) : RGB(96, 60, 148), glow);
  frame_ellipse(canvas, f, -0.05f, 0.0f, 0.36f, 0.27f, darken(shell, 90u),
                256u);
  frame_ellipse(canvas, f, -0.05f, 0.0f, 0.33f, 0.24f, shell, 256u);
  frame_capsule(canvas, f, -0.36f, 0.0f, 0.2f, 0.0f, 0.02f, darken(shell, 150u),
                256u);
  frame_disc(canvas, f, -0.14f, -0.12f, 0.07f, lighten(shell, 120u), 150u);
  frame_disc(canvas, f, -0.14f, 0.12f, 0.07f, lighten(shell, 120u), 150u);
  if (armored) {
    const uint16_t steel = lighten(RGB(160, 168, 182), glow);
    frame_ellipse(canvas, f, -0.08f, 0.0f, 0.3f, 0.24f, darken(steel, 70u),
                  256u);
    frame_ellipse(canvas, f, -0.08f, 0.0f, 0.27f, 0.21f, steel, 256u);
    frame_capsule(canvas, f, -0.32f, 0.0f, 0.16f, 0.0f, 0.03f,
                  lighten(steel, 120u), 220u);
    for (int side = -1; side <= 1; side += 2)
      for (int k = -1; k <= 1; ++k)
        frame_disc(canvas, f, (float)k * 0.12f - 0.08f, (float)side * 0.15f,
                   0.025f, darken(steel, 120u), 256u);
  }
  frame_disc(canvas, f, 0.3f + bob, 0.0f, 0.15f, head, 256u);
  frame_disc(canvas, f, 0.38f + bob, -0.075f, 0.04f, RGB(250, 246, 220), 256u);
  frame_disc(canvas, f, 0.38f + bob, 0.075f, 0.04f, RGB(250, 246, 220), 256u);
  frame_disc(canvas, f, 0.4f + bob, -0.075f, 0.02f, RGB(16, 12, 12), 256u);
  frame_disc(canvas, f, 0.4f + bob, 0.075f, 0.02f, RGB(16, 12, 12), 256u);
}

static void draw_runner(h2_garden_guard_canvas_t* canvas, const frame_t* f,
                        float t, bool eating, bool flash) {
  const uint32_t glow = flash ? 140u : 0u;
  const float wiggle = eating ? 0.0f : sinf(t * 22.0f) * 0.1f;
  const float bob = eating ? sinf(t * 18.0f) * 0.05f : 0.0f;
  const uint16_t legs = lighten(RGB(60, 20, 14), glow);
  for (int k = -1; k <= 1; ++k) {
    for (int side = -1; side <= 1; side += 2) {
      const float swing = ((k + side) & 2) != 0 ? wiggle : -wiggle;
      frame_capsule(canvas, f, -0.02f, (float)side * 0.06f,
                    (float)k * 0.22f + swing, (float)side * 0.4f, 0.03f, legs,
                    256u);
    }
  }
  const uint16_t body = lighten(RGB(196, 62, 38), glow);
  frame_capsule(canvas, f, 0.3f + bob, -0.05f, 0.5f + bob, -0.18f, 0.02f, legs,
                256u);
  frame_capsule(canvas, f, 0.3f + bob, 0.05f, 0.5f + bob, 0.18f, 0.02f, legs,
                256u);
  frame_ellipse(canvas, f, -0.28f, 0.0f, 0.22f, 0.17f, darken(body, 40u), 256u);
  frame_disc(canvas, f, -0.32f, -0.06f, 0.06f, lighten(body, 110u), 150u);
  frame_disc(canvas, f, 0.0f, 0.0f, 0.1f, body, 256u);
  frame_disc(canvas, f, 0.2f + bob, 0.0f, 0.13f, body, 256u);
  frame_disc(canvas, f, 0.26f + bob, -0.06f, 0.035f, RGB(20, 10, 8), 256u);
  frame_disc(canvas, f, 0.26f + bob, 0.06f, 0.035f, RGB(20, 10, 8), 256u);
}

static void draw_cart(h2_garden_guard_canvas_t* canvas, const frame_t* f) {
  frame_capsule(canvas, f, -0.32f, -0.28f, -0.32f, 0.28f, 0.07f,
                RGB(30, 30, 34), 256u);
  frame_capsule(canvas, f, 0.24f, -0.28f, 0.24f, 0.28f, 0.07f, RGB(30, 30, 34),
                256u);
  float x0, y0, x1, y1;
  frame_point(f, -0.38f, -0.24f, &x0, &y0);
  frame_point(f, 0.36f, 0.24f, &x1, &y1);
  rounded_rect(canvas, fminf(x0, x1), fminf(y0, y1), fmaxf(x0, x1),
               fmaxf(y0, y1), 0.08f * f->s, RGB(196, 46, 36), 256u);
  frame_point(f, -0.28f, -0.15f, &x0, &y0);
  frame_point(f, 0.26f, 0.15f, &x1, &y1);
  rounded_rect(canvas, fminf(x0, x1), fminf(y0, y1), fmaxf(x0, x1),
               fmaxf(y0, y1), 0.05f * f->s, RGB(150, 30, 26), 256u);
  frame_capsule(canvas, f, -0.38f, -0.12f, -0.62f, -0.12f, 0.035f,
                RGB(90, 60, 36), 256u);
  frame_capsule(canvas, f, -0.38f, 0.12f, -0.62f, 0.12f, 0.035f,
                RGB(90, 60, 36), 256u);
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

void h2_garden_guard_render_background(h2_garden_guard_canvas_t* canvas,
                                       const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  region_t g;
  if (!region_begin(canvas, 0.0f, 0.0f, (float)canvas->width,
                    (float)canvas->height, &g))
    return;
  const float field_x1 = l->field_x + l->field_w;
  const float field_y1 = l->field_y + l->field_h;
  float stone = 18.0f * l->unit;
  if (stone < 6.0f) stone = 6.0f;
  const float plank = fmaxf(l->bar_h * 0.5f, 4.0f);
  for (int32_t y = g.y0; y < g.y1; ++y) {
    uint16_t* row = row_at(canvas, y);
    const float fy = (float)y + 0.5f;
    for (int32_t x = g.x0; x < g.x1; ++x) {
      const float fx = (float)x + 0.5f;
      const uint32_t noise =
          hash32((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ 0x5A17u);
      uint16_t color;
      if (fy < l->bar_h) {
        /* Toolbar: two wooden planks with flowing grain. */
        const int32_t index = (int32_t)(fy / plank);
        const float grain =
            sinf(fx * 0.045f + (float)index * 1.7f + 2.0f * sinf(fx * 0.011f));
        const float k = 1.0f + 0.07f * grain +
                        0.04f * ((float)(noise & 15u) / 15.0f - 0.5f);
        color = RGB((unsigned)clampf(98.0f * k, 0.0f, 255.0f),
                    (unsigned)clampf(64.0f * k, 0.0f, 255.0f),
                    (unsigned)clampf(38.0f * k, 0.0f, 255.0f));
        if (fy > l->bar_h - 3.0f || fmodf(fy, plank) < 1.0f)
          color = RGB(54, 34, 20);
      } else if (fx >= l->field_x && fx < field_x1 && fy < field_y1) {
        /* Lawn: a checkerboard of cells with speckled blades. */
        uint32_t lane, col;
        bool odd = false;
        if (h2_garden_guard_cell_at(l, fx, fy, &lane, &col))
          odd = ((lane + col) & 1u) != 0u;
        color = odd ? RGB(98, 164, 60) : RGB(114, 180, 72);
        const uint32_t speck = noise & 63u;
        if (speck < 3u)
          color = darken(color, 60u);
        else if (speck < 5u)
          color = lighten(color, 50u);
      } else {
        /* House edge: a stone path in a running bond. */
        const int32_t course = (int32_t)(fy / stone);
        const float shifted = fx + ((course & 1) ? stone * 0.5f : 0.0f);
        const int32_t brick = (int32_t)(shifted / stone);
        const uint32_t tone =
            hash32((uint32_t)course * 2654435761u ^ (uint32_t)brick) & 31u;
        color = RGB(184u + tone / 2u, 160u + tone / 2u, 124u + tone / 3u);
        if (fmodf(fy, stone) < 1.5f || fmodf(shifted, stone) < 1.5f)
          color = RGB(132, 112, 86);
      }
      row[x] = color;
    }
  }
}

static void field_clip(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_layout_t* l) {
  canvas->clip_y0 = (int32_t)ceilf(l->bar_h);
  canvas->clip_y1 = canvas->height;
}

static void full_clip(h2_garden_guard_canvas_t* canvas) {
  canvas->clip_y0 = 0;
  canvas->clip_y1 = 0;
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
  for (unsigned lane = 0; lane < H2_GARDEN_GUARD_LANES; ++lane)
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
               affordable ? RGB(116, 176, 82) : RGB(112, 120, 108), 256u);
  const float cx = (x0 + x1) * 0.5f, cy = (y0 + inset + well_y1) * 0.5f;
  const float size = fminf(l->card_w - 2.0f * inset, well_y1 - y0 - inset);
  if (card == H2_GARDEN_GUARD_CARD_SHOVEL) {
    draw_shovel(canvas, cx, cy, size);
  } else {
    float fx, fy;
    plant_facing(l, &fx, &fy);
    const frame_t f = {cx, cy, size * 1.02f, fx, fy};
    draw_plant_kind(canvas, (uint8_t)(card + 1u), &f, 0u);
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

static void panel(h2_garden_guard_canvas_t* canvas,
                  const h2_garden_guard_layout_t* l, float width_fraction,
                  float height_fraction, float* out_x0, float* out_y0,
                  float* out_w, float* out_h) {
  const float w = fminf(l->field_w * width_fraction, 560.0f * l->unit);
  const float h = fminf(l->field_h * height_fraction, 400.0f * l->unit);
  const float x0 = l->field_x + (l->field_w - w) * 0.5f;
  const float y0 = l->field_y + (l->field_h - h) * 0.5f;
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
  panel(canvas, l, 0.92f, 0.9f, &x0, &y0, &w, &h);
  const float cx = x0 + w * 0.5f;
  const int32_t big = fit_scale("GARDEN GUARD", w * 0.9f, text_scale(u, 6.0f));
  text_centered(canvas, "GARDEN GUARD", cx, y0 + h * 0.07f, big,
                RGB(255, 214, 60), 256u);
  const float s = fminf(w / 5.2f, h * 0.24f);
  const float row = y0 + h * 0.43f;
  const frame_t sunflower = {x0 + w * 0.17f, row, s, 1.0f, 0.0f};
  const frame_t shooter = {x0 + w * 0.39f, row, s, 1.0f, 0.0f};
  const frame_t pumpkin = {x0 + w * 0.61f, row, s, 1.0f, 0.0f};
  const frame_t beetle = {x0 + w * 0.83f, row, s * 0.9f, -1.0f, 0.0f};
  draw_sunflower(canvas, &sunflower, 0u);
  draw_shooter(canvas, &shooter, 0u);
  draw_pumpkin(canvas, &pumpkin, 0u);
  draw_beetle(canvas, &beetle, 0.0f, false, false, false, false);
  const char* hint1 = "TAP SUN TO COLLECT IT";
  const char* hint2 = "PICK A CARD, THEN A TILE";
  const int32_t hint = fit_scale(hint2, w * 0.92f, text_scale(u, 2.6f));
  text_centered(canvas, hint1, cx, y0 + h * 0.62f, hint, RGB(232, 240, 214),
                230u);
  text_centered(canvas, hint2, cx, y0 + h * 0.62f + (float)(hint * 10), hint,
                RGB(232, 240, 214), 230u);
  const int32_t start =
      fit_scale("TAP TO START", w * 0.9f, text_scale(u, 3.0f));
  /* Keep a gap below the hints when the panel is short. */
  const float start_y =
      fmaxf(y0 + h * 0.79f, y0 + h * 0.62f + (float)(hint * 20) + 6.0f * u);
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
  panel(canvas, l, 0.86f, 0.76f, &x0, &y0, &w, &h);
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

static void draw_field(h2_garden_guard_canvas_t* canvas,
                       const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  float fx, fy;
  plant_facing(l, &fx, &fy);
  field_clip(canvas, l);
  for (uint32_t lane = 0; lane < H2_GARDEN_GUARD_LANES; ++lane) {
    for (uint32_t col = 0; col < H2_GARDEN_GUARD_COLS; ++col) {
      const h2_garden_guard_plant_t* plant = &game->plants[lane][col];
      if (plant->kind == H2_GARDEN_GUARD_PLANT_NONE) continue;
      float x, y;
      h2_garden_guard_field_point(l, lane, (float)col + 0.5f, &x, &y);
      const frame_t f = {x, y, l->sprite * 0.9f, fx, fy};
      draw_plant_kind(canvas, plant->kind, &f, plant->stage);
    }
    if (game->carts[lane].state == H2_GARDEN_GUARD_CART_READY) {
      float x, y;
      h2_garden_guard_field_point(l, lane, -0.2f, &x, &y);
      const frame_t f = {x, y, l->sprite * 0.5f, fx, fy};
      draw_cart(canvas, &f);
    }
  }
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
  if (part == H2_GARDEN_GUARD_PART_TOOLBAR)
    draw_toolbar(canvas, game);
  else
    draw_field(canvas, game);
}

static uint32_t fade(uint32_t age, uint32_t life, uint32_t peak) {
  if (age >= life || life == 0u) return 0u;
  return peak * (life - age) / life;
}

void h2_garden_guard_render_scene(h2_garden_guard_canvas_t* canvas,
                                  const h2_garden_guard_game_t* game) {
  const h2_garden_guard_layout_t* l = &game->layout;
  const float u = l->unit;
  float fx, fy;
  plant_facing(l, &fx, &fy);
  field_clip(canvas, l);
  for (uint32_t lane = 0; lane < H2_GARDEN_GUARD_LANES; ++lane) {
    const h2_garden_guard_cart_t* cart = &game->carts[lane];
    if (cart->state != H2_GARDEN_GUARD_CART_SWEEPING) continue;
    float x, y;
    h2_garden_guard_field_point(l, lane, cart->x, &x, &y);
    const frame_t f = {x, y, l->sprite * 0.5f, fx, fy};
    draw_cart(canvas, &f);
  }
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i) {
    const h2_garden_guard_pest_t* pest = &game->pests[i];
    if (!pest->active) continue;
    float x, y;
    h2_garden_guard_field_point(l, pest->lane, pest->x, &x, &y);
    const float t =
        (float)((game->time_ms + pest->phase_ms) % 600000u) * 0.001f;
    const bool flash = pest->flash_ms != 0u;
    if (pest->kind == H2_GARDEN_GUARD_PEST_RUNNER) {
      const frame_t f = {x, y, l->sprite * 0.7f, -fx, -fy};
      draw_runner(canvas, &f, t, pest->eating, flash);
    } else {
      const bool shellback = pest->kind == H2_GARDEN_GUARD_PEST_SHELLBACK;
      const frame_t f = {x, y, l->sprite * (shellback ? 0.86f : 0.8f), -fx,
                         -fy};
      draw_beetle(canvas, &f, t, pest->eating, shellback,
                  shellback && pest->armor > 0, flash);
    }
  }
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PEAS; ++i) {
    const h2_garden_guard_pea_t* pea = &game->peas[i];
    if (!pea->active) continue;
    float x, y;
    h2_garden_guard_field_point(l, pea->lane, pea->x, &x, &y);
    const float r = l->sprite * 0.1f;
    disc(canvas, x, y, r * 1.15f, RGB(40, 104, 30), 256u);
    disc(canvas, x, y, r, RGB(140, 222, 84), 256u);
    disc(canvas, x - r * 0.3f, y - r * 0.3f, r * 0.35f, RGB(220, 255, 190),
         200u);
  }
  full_clip(canvas);

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
    draw_sun(canvas, sun->x, sun->y, sun->collecting ? sun_r * 0.8f : sun_r,
             spin, 256u);
  }

  if (game->banner_ms > 0u && game->banner[0] != '\0') {
    const uint32_t alpha =
        game->banner_ms >= 500u ? 256u : game->banner_ms * 256u / 500u;
    const int32_t scale =
        fit_scale(game->banner, l->field_w * 0.86f, text_scale(u, 5.0f));
    text_centered(canvas, game->banner, l->field_x + l->field_w * 0.5f,
                  l->field_y + l->field_h * 0.5f - 3.5f * (float)scale, scale,
                  RGB(255, 238, 120), alpha);
  }
}
