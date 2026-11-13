#include "h2_fruit_slice_render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BAND_ROWS H2_FRUIT_SLICE_BAND_ROWS

typedef struct region {
  int32_t x0, y0, x1, y1; /* Half-open, clipped to the canvas. */
} region_t;

typedef struct color3 {
  float r, g, b;
} color3_t;

typedef struct palette {
  color3_t skin, skin_alt, pith, flesh, flesh_alt;
  float rind, pith_width; /* Fractions of the radius. */
} palette_t;

static const palette_t k_palette[H2_FRUIT_SLICE_KIND_COUNT] = {
    [H2_FRUIT_SLICE_WATERMELON] = {{40, 116, 44},
                                   {98, 172, 70},
                                   {228, 242, 204},
                                   {236, 48, 70},
                                   {250, 96, 104},
                                   0.12f,
                                   0.06f},
    [H2_FRUIT_SLICE_ORANGE] = {{255, 136, 16},
                               {255, 172, 64},
                               {255, 238, 204},
                               {255, 166, 46},
                               {255, 200, 96},
                               0.08f,
                               0.05f},
    [H2_FRUIT_SLICE_APPLE] = {{206, 30, 40},
                              {242, 132, 44},
                              {252, 240, 206},
                              {252, 240, 200},
                              {238, 222, 168},
                              0.07f,
                              0.0f},
    [H2_FRUIT_SLICE_KIWI] = {{122, 88, 56},
                             {152, 114, 74},
                             {150, 210, 70},
                             {118, 192, 52},
                             {238, 242, 212},
                             0.06f,
                             0.0f},
    [H2_FRUIT_SLICE_LEMON] = {{250, 214, 36},
                              {255, 240, 112},
                              {255, 250, 226},
                              {255, 232, 98},
                              {255, 246, 172},
                              0.08f,
                              0.05f},
    [H2_FRUIT_SLICE_BOMB] = {{36, 40, 48},
                             {68, 74, 88},
                             {0, 0, 0},
                             {0, 0, 0},
                             {0, 0, 0},
                             0.0f,
                             0.0f},
};

static const color3_t k_white = {255, 255, 255};

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
static const uint8_t k_minus[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
static const uint8_t k_colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
static const uint8_t k_bang[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
static const uint8_t k_dot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
static const uint8_t k_times[7] = {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11};

static const uint8_t* glyph(char c) {
  if (c >= '0' && c <= '9') return k_digits[c - '0'];
  if (c >= 'A' && c <= 'Z') return k_letters[c - 'A'];
  if (c >= 'a' && c <= 'z') return c == 'x' ? k_times : k_letters[c - 'a'];
  switch (c) {
    case '+':
      return k_plus;
    case '-':
      return k_minus;
    case ':':
      return k_colon;
    case '!':
      return k_bang;
    case '.':
      return k_dot;
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

static color3_t mix(color3_t a, color3_t b, float t) {
  return (color3_t){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                    a.b + (b.b - a.b) * t};
}

static color3_t scaled(color3_t a, float k) {
  return (color3_t){a.r * k, a.g * k, a.b * k};
}

static uint16_t pack(color3_t c) {
  return H2_FRUIT_SLICE_RGB((unsigned)clampf(c.r, 0.0f, 255.0f),
                            (unsigned)clampf(c.g, 0.0f, 255.0f),
                            (unsigned)clampf(c.b, 0.0f, 255.0f));
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

uint64_t h2_fruit_slice_all_bands(int32_t height) {
  if (height <= 0) return 0;
  const int32_t bands = (height + BAND_ROWS - 1) / BAND_ROWS;
  if (bands >= H2_FRUIT_SLICE_MAX_BANDS) return UINT64_MAX;
  return ((uint64_t)1 << bands) - 1u;
}

/* Clip a float box, record its bands and report whether to paint it. */
static bool region_begin(h2_fruit_slice_canvas_t* canvas, float fx0, float fy0,
                         float fx1, float fy1, region_t* out) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const int32_t x0 = (int32_t)floorf(clampf(fx0, 0.0f, w));
  const int32_t y0 = (int32_t)floorf(clampf(fy0, 0.0f, h));
  const int32_t x1 = (int32_t)ceilf(clampf(fx1, 0.0f, w));
  const int32_t y1 = (int32_t)ceilf(clampf(fy1, 0.0f, h));
  if (x0 >= x1 || y0 >= y1) return false;
  for (int32_t band = y0 / BAND_ROWS;
       band <= (y1 - 1) / BAND_ROWS && band < H2_FRUIT_SLICE_MAX_BANDS;
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

static uint16_t* row_at(const h2_fruit_slice_canvas_t* canvas, int32_t y) {
  return canvas->pixels + (size_t)y * canvas->stride_pixels;
}

static void fill_rect(h2_fruit_slice_canvas_t* canvas, int32_t x, int32_t y,
                      int32_t w, int32_t h, uint16_t color, uint32_t alpha) {
  region_t g;
  if (!region_begin(canvas, (float)x, (float)y, (float)(x + w), (float)(y + h),
                    &g))
    return;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    for (int32_t px = g.x0; px < g.x1; ++px)
      row[px] = blend(row[px], color, alpha);
  }
}

static void disc(h2_fruit_slice_canvas_t* canvas, float cx, float cy, float r,
                 uint16_t color, uint32_t alpha) {
  if (!(r > 0.0f)) return;
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

static void capsule(h2_fruit_slice_canvas_t* canvas, float x0, float y0,
                    float x1, float y1, float r, uint16_t color,
                    uint32_t alpha) {
  if (!(r > 0.0f)) return;
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

static void rounded_rect(h2_fruit_slice_canvas_t* canvas, float x0, float y0,
                         float x1, float y1, float radius, uint16_t color,
                         uint32_t alpha) {
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

int32_t h2_fruit_slice_text_width(const char* text, int32_t scale) {
  const size_t length = text != NULL ? strlen(text) : 0u;
  if (length == 0u || scale <= 0) return 0;
  return (int32_t)length * 6 * scale - scale;
}

static void text_layer(h2_fruit_slice_canvas_t* canvas, const char* text,
                       int32_t x, int32_t y, int32_t scale, uint16_t color,
                       uint32_t alpha) {
  for (const char* c = text; *c != '\0'; ++c, x += 6 * scale) {
    const uint8_t* rows = glyph(*c);
    if (rows == NULL) continue;
    for (int32_t row = 0; row < 7; ++row)
      for (int32_t column = 0; column < 5; ++column)
        if (rows[row] & (0x10u >> column))
          fill_rect(canvas, x + column * scale, y + row * scale, scale, scale,
                    color, alpha);
  }
}

/* Text with a soft drop shadow; (x, y) is the top-left corner. */
static void text(h2_fruit_slice_canvas_t* canvas, const char* value, int32_t x,
                 int32_t y, int32_t scale, uint16_t color, uint32_t alpha) {
  const int32_t offset = scale / 2 > 0 ? scale / 2 : 1;
  text_layer(canvas, value, x + offset, y + offset, scale,
             H2_FRUIT_SLICE_RGB(28, 16, 8), alpha * 3u / 4u);
  text_layer(canvas, value, x, y, scale, color, alpha);
}

static void text_centered(h2_fruit_slice_canvas_t* canvas, const char* value,
                          float cx, float y, int32_t scale, uint16_t color,
                          uint32_t alpha) {
  const int32_t width = h2_fruit_slice_text_width(value, scale);
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
         (float)h2_fruit_slice_text_width(value, scale) > max_width)
    --scale;
  return scale;
}

void h2_fruit_slice_render_background(h2_fruit_slice_canvas_t* canvas) {
  region_t g;
  if (!region_begin(canvas, 0.0f, 0.0f, (float)canvas->width,
                    (float)canvas->height, &g))
    return;
  const int32_t short_side =
      canvas->width < canvas->height ? canvas->width : canvas->height;
  int32_t plank = short_side / 6;
  if (plank < 24) plank = 24;
  const float half_w = (float)canvas->width * 0.5f;
  const float half_h = (float)canvas->height * 0.5f;
  for (int32_t y = g.y0; y < g.y1; ++y) {
    uint16_t* row = row_at(canvas, y);
    const float vy = ((float)y + 0.5f - half_h) / half_h;
    for (int32_t x = g.x0; x < g.x1; ++x) {
      const int32_t index = x / plank, inside = x % plank;
      const uint32_t seed = hash32((uint32_t)index * 2654435761u + 17u);
      const float tone = (float)(seed & 255u) / 255.0f;
      color3_t wood = {112.0f + 26.0f * tone, 70.0f + 14.0f * tone,
                       40.0f + 8.0f * tone};
      const float phase = (float)(seed >> 8 & 1023u) * 0.01f;
      const float grain =
          sinf((float)y * 0.045f + phase +
               2.4f * sinf((float)y * 0.012f + (float)inside * 0.05f + phase));
      const float fleck = (float)(hash32((uint32_t)x * 73856093u ^
                                         (uint32_t)(y / 3) * 19349663u) &
                                  15u) /
                          15.0f;
      wood = scaled(wood, 1.0f + 0.10f * grain + 0.05f * (fleck - 0.5f));
      if (inside < 2)
        wood = (color3_t){58, 34, 20};
      else if (inside == 2)
        wood = scaled(wood, 1.18f);
      const float vx = ((float)x + 0.5f - half_w) / half_w;
      const float vignette =
          1.0f - 0.32f * clampf((vx * vx + vy * vy) * 0.5f, 0.0f, 1.0f);
      row[x] = pack(scaled(wood, vignette));
    }
  }
}

/* Screen position of a point given in the fruit's rotated unit frame. */
static void local_point(float cx, float cy, float r, float ca, float sa,
                        float a, float b, float* x, float* y) {
  *x = cx + (a * ca - b * sa) * r;
  *y = cy + (a * sa + b * ca) * r;
}

/* Unlit skin color at fruit-local unit coordinates (ru, rv); r in pixels. */
static color3_t whole_albedo(h2_fruit_slice_kind_t kind, float ru, float rv,
                             float r) {
  const palette_t* p = &k_palette[kind];
  color3_t base = p->skin;
  if (kind == H2_FRUIT_SLICE_WATERMELON) {
    const float s = ru / sqrtf(fmaxf(0.04f, 1.0f - rv * rv));
    const float stripe = sinf(s * 5.2f + 0.35f * sinf(rv * 6.0f));
    base = stripe > 0.15f ? p->skin : p->skin_alt;
  } else if (kind == H2_FRUIT_SLICE_APPLE) {
    base = mix(p->skin, p->skin_alt, clampf((ru - 0.15f) * 1.3f, 0.0f, 0.55f));
  } else if (kind != H2_FRUIT_SLICE_BOMB) {
    const uint32_t qx = (uint32_t)(int32_t)((ru + 2.0f) * r * 0.6f);
    const uint32_t qy = (uint32_t)(int32_t)((rv + 2.0f) * r * 0.6f);
    const float noise =
        (float)(hash32(qx * 73856093u ^ qy * 19349663u) & 31u) / 31.0f - 0.5f;
    base = scaled(base,
                  1.0f + (kind == H2_FRUIT_SLICE_KIWI ? 0.22f : 0.08f) * noise);
  }
  return base;
}

static void draw_whole_details(h2_fruit_slice_canvas_t* canvas,
                               h2_fruit_slice_kind_t kind, float cx, float cy,
                               float r, float ca, float sa, uint64_t time_ms) {
  float x, y, x1, y1;
  switch (kind) {
    case H2_FRUIT_SLICE_ORANGE:
      local_point(cx, cy, r, ca, sa, 0.0f, -0.86f, &x, &y);
      disc(canvas, x, y, 0.11f * r, H2_FRUIT_SLICE_RGB(70, 120, 40), 256u);
      break;
    case H2_FRUIT_SLICE_APPLE:
      local_point(cx, cy, r, ca, sa, 0.0f, -0.80f, &x, &y);
      local_point(cx, cy, r, ca, sa, 0.06f, -1.14f, &x1, &y1);
      capsule(canvas, x, y, x1, y1, 0.055f * r, H2_FRUIT_SLICE_RGB(92, 56, 28),
              256u);
      local_point(cx, cy, r, ca, sa, 0.26f, -1.0f, &x, &y);
      disc(canvas, x, y, 0.17f * r, H2_FRUIT_SLICE_RGB(78, 160, 48), 256u);
      break;
    case H2_FRUIT_SLICE_BOMB: {
      local_point(cx, cy, r, ca, sa, 0.22f, -0.86f, &x, &y);
      local_point(cx, cy, r, ca, sa, 0.42f, -1.24f, &x1, &y1);
      capsule(canvas, x, y, x1, y1, 0.08f * r, H2_FRUIT_SLICE_RGB(156, 126, 84),
              256u);
      const bool flicker = (time_ms / 70u) % 2u == 0u;
      disc(canvas, x1, y1, 0.42f * r, H2_FRUIT_SLICE_RGB(255, 112, 32), 80u);
      disc(canvas, x1, y1, (flicker ? 0.22f : 0.15f) * r,
           flicker ? H2_FRUIT_SLICE_RGB(255, 220, 90)
                   : H2_FRUIT_SLICE_RGB(255, 150, 40),
           256u);
      /* A red band reads as danger at a glance. */
      local_point(cx, cy, r, ca, sa, -0.5f, 0.5f, &x, &y);
      local_point(cx, cy, r, ca, sa, 0.5f, -0.5f, &x1, &y1);
      capsule(canvas, x, y, x1, y1, 0.09f * r, H2_FRUIT_SLICE_RGB(220, 40, 40),
              230u);
      local_point(cx, cy, r, ca, sa, -0.5f, -0.5f, &x, &y);
      local_point(cx, cy, r, ca, sa, 0.5f, 0.5f, &x1, &y1);
      capsule(canvas, x, y, x1, y1, 0.09f * r, H2_FRUIT_SLICE_RGB(220, 40, 40),
              230u);
      break;
    }
    default:
      break;
  }
}

typedef struct seed_point {
  float a, b;
} seed_point_t;

static const seed_point_t k_melon_seeds[] = {
    {0.28f, -0.38f}, {0.30f, 0.36f}, {0.48f, -0.08f}, {0.50f, 0.22f},
    {0.60f, -0.40f}, {0.62f, 0.46f}, {0.22f, 0.02f},
};

static bool near_seed(const seed_point_t* seeds, size_t count, float a, float b,
                      float radius2) {
  for (size_t i = 0; i < count; ++i) {
    const float da = a - seeds[i].a, db = b - seeds[i].b;
    if (da * da + db * db < radius2) return true;
  }
  return false;
}

static color3_t flesh_color(h2_fruit_slice_kind_t kind, const palette_t* p,
                            float a, float b, float nd) {
  color3_t color = mix(p->flesh, p->flesh_alt, nd * 0.5f);
  switch (kind) {
    case H2_FRUIT_SLICE_WATERMELON:
      color = mix(p->flesh_alt, p->flesh, clampf(nd * 1.6f, 0.0f, 1.0f));
      if (near_seed(k_melon_seeds,
                    sizeof(k_melon_seeds) / sizeof(k_melon_seeds[0]), a, b,
                    0.0028f))
        color = (color3_t){34, 22, 22};
      break;
    case H2_FRUIT_SLICE_ORANGE:
    case H2_FRUIT_SLICE_LEMON: {
      static const float directions[][2] = {{0.5f, -0.866f},
                                            {0.866f, -0.5f},
                                            {1.0f, 0.0f},
                                            {0.866f, 0.5f},
                                            {0.5f, 0.866f}};
      for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
        const float cross = a * directions[i][1] - b * directions[i][0];
        const float along = a * directions[i][0] + b * directions[i][1];
        if (fabsf(cross) < 0.024f && along > 0.06f) color = p->pith;
      }
      if (a * a + b * b < 0.006f) color = p->pith;
      break;
    }
    case H2_FRUIT_SLICE_APPLE: {
      static const seed_point_t seeds[] = {{0.20f, -0.10f}, {0.20f, 0.10f}};
      if (a * a + b * b < 0.10f) color = p->flesh_alt;
      if (near_seed(seeds, 2u, a, b, 0.0032f)) color = (color3_t){96, 52, 24};
      break;
    }
    case H2_FRUIT_SLICE_KIWI: {
      static const seed_point_t seeds[] = {
          {0.069f, -0.394f}, {0.200f, -0.346f}, {0.306f, -0.257f},
          {0.376f, -0.137f}, {0.400f, 0.000f},  {0.376f, 0.137f},
          {0.306f, 0.257f},  {0.200f, 0.346f},  {0.069f, 0.394f},
      };
      const float center = a * a + b * b;
      color = mix(p->pith, p->flesh, clampf(nd * 1.4f, 0.0f, 1.0f));
      if (center < 0.045f) color = p->flesh_alt;
      if (near_seed(seeds, sizeof(seeds) / sizeof(seeds[0]), a, b, 0.0016f))
        color = (color3_t){24, 20, 16};
      break;
    }
    default:
      break;
  }
  return color;
}

/* Cut-face color at half-local pixel coordinates: a_px from the cut along the
 * normal, b_px along the cut; r in pixels. */
static color3_t half_color(h2_fruit_slice_kind_t kind, float a_px, float b_px,
                           float r) {
  const palette_t* p = &k_palette[kind];
  const float a = a_px / r, b = b_px / r;
  const float nd = sqrtf(a * a + b * b), edge = 1.0f - nd;
  color3_t color;
  if (edge < 0.035f)
    color = scaled(p->skin, 0.6f);
  else if (edge < p->rind)
    color = p->skin;
  else if (edge < p->rind + p->pith_width)
    color = p->pith;
  else
    color = scaled(flesh_color(kind, p, a, b, nd), 1.0f - 0.10f * nd);
  if (a_px < 2.5f && edge >= p->rind) color = mix(color, k_white, 0.28f);
  return color;
}

typedef struct sprite {
  uint8_t kind;
  bool half;
  uint32_t radius_eighths; /* Cache key: radius in 1/8 pixel. */
  float radius;
  int32_t extent; /* Texel offset of the fruit center. */
  int32_t width, height;
  uint16_t* texels;
} sprite_t;

struct h2_fruit_slice_sprites {
  const h2_pal_mem_api_t* allocator;
  size_t count;
  sprite_t entries[H2_FRUIT_SLICE_SPRITE_CAPACITY];
};

h2_fruit_slice_sprites_t* h2_fruit_slice_sprites_create(
    const h2_pal_mem_api_t* allocator) {
  h2_fruit_slice_sprites_t* sprites =
      h2_pal_mem_alloc(allocator, sizeof(*sprites));
  if (sprites == NULL) return NULL;
  memset(sprites, 0, sizeof(*sprites));
  sprites->allocator = allocator;
  return sprites;
}

void h2_fruit_slice_sprites_destroy(h2_fruit_slice_sprites_t* sprites) {
  if (sprites == NULL) return;
  for (size_t i = 0; i < sprites->count; ++i)
    h2_pal_mem_free(sprites->allocator, sprites->entries[i].texels);
  h2_pal_mem_free(sprites->allocator, sprites);
}

/* Find or build one texture; NULL when unavailable (cache full, no memory or
 * an unusable radius). */
static const sprite_t* sprite_get(h2_fruit_slice_sprites_t* sprites,
                                  h2_fruit_slice_kind_t kind, bool half,
                                  float radius) {
  if (sprites == NULL || !(radius >= 1.0f && radius <= 512.0f) ||
      (unsigned)kind >= H2_FRUIT_SLICE_KIND_COUNT)
    return NULL;
  const uint32_t key = (uint32_t)(radius * 8.0f + 0.5f);
  for (size_t i = 0; i < sprites->count; ++i) {
    const sprite_t* s = &sprites->entries[i];
    if (s->kind == kind && s->half == half && s->radius_eighths == key)
      return s;
  }
  if (sprites->count == H2_FRUIT_SLICE_SPRITE_CAPACITY) return NULL;
  const float r = (float)key / 8.0f;
  const int32_t extent = (int32_t)ceilf(r) + 1;
  const int32_t width = half ? extent + 1 : 2 * extent + 1;
  const int32_t height = 2 * extent + 1;
  uint16_t* texels = h2_pal_mem_alloc(
      sprites->allocator, (size_t)width * (size_t)height * sizeof(uint16_t));
  if (texels == NULL) return NULL;
  for (int32_t ty = 0; ty < height; ++ty)
    for (int32_t tx = 0; tx < width; ++tx) {
      const float ly = (float)(ty - extent);
      const color3_t color =
          half ? half_color(kind, (float)tx, ly, r)
               : whole_albedo(kind, (float)(tx - extent) / r, ly / r, r);
      texels[(size_t)ty * (size_t)width + (size_t)tx] = pack(color);
    }
  sprite_t* s = &sprites->entries[sprites->count++];
  *s = (sprite_t){.kind = (uint8_t)kind,
                  .half = half,
                  .radius_eighths = key,
                  .radius = r,
                  .extent = extent,
                  .width = width,
                  .height = height,
                  .texels = texels};
  return s;
}

bool h2_fruit_slice_sprites_prepare(h2_fruit_slice_sprites_t* sprites,
                                    const h2_fruit_slice_game_t* game) {
  bool ok = sprites != NULL;
  for (unsigned k = 0; ok && k < H2_FRUIT_SLICE_KIND_COUNT; ++k) {
    const h2_fruit_slice_kind_t kind = (h2_fruit_slice_kind_t)k;
    const float r = h2_fruit_slice_game_radius(game, kind);
    ok = sprite_get(sprites, kind, false, r) != NULL;
    if (ok && kind != H2_FRUIT_SLICE_BOMB)
      ok = sprite_get(sprites, kind, true, r) != NULL;
  }
  /* Menu fruits and their halves, then the HUD icon. */
  static const h2_fruit_slice_kind_t menus[] = {H2_FRUIT_SLICE_WATERMELON,
                                                H2_FRUIT_SLICE_ORANGE};
  for (unsigned i = 0; ok && i < sizeof(menus) / sizeof(menus[0]); ++i) {
    const float r =
        h2_fruit_slice_game_radius(game, menus[i]) * H2_FRUIT_SLICE_MENU_SCALE;
    ok = sprite_get(sprites, menus[i], false, r) != NULL &&
         sprite_get(sprites, menus[i], true, r) != NULL;
  }
  return ok && sprite_get(sprites, H2_FRUIT_SLICE_WATERMELON, true,
                          20.0f * game->unit) != NULL;
}

static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }

/* Scale by light (1/256 units) and mix toward white by highlight (1/256). */
static uint16_t shade(uint16_t color, uint32_t light, uint32_t highlight) {
  uint32_t r = expand5(color >> 11) * light >> 8;
  uint32_t g = expand6((color >> 5) & 63u) * light >> 8;
  uint32_t b = expand5(color & 31u) * light >> 8;
  if (r > 255u) r = 255u;
  if (g > 255u) g = 255u;
  if (b > 255u) b = 255u;
  r += (255u - r) * highlight >> 8;
  g += (255u - g) * highlight >> 8;
  b += (255u - b) * highlight >> 8;
  return H2_FRUIT_SLICE_RGB(r, g, b);
}

static int32_t texel_index(float value, int32_t extent, int32_t limit) {
  int32_t index = (int32_t)(value + (float)extent + 0.5f);
  if (index < 0) index = 0;
  if (index >= limit) index = limit - 1;
  return index;
}

/* Clip the disc of squared radius outer to the region row at dy. */
static bool disc_span(const region_t* g, float cx, float dy2, float outer,
                      int32_t* x0, int32_t* x1) {
  if (dy2 >= outer) return false;
  const float span = sqrtf(outer - dy2);
  *x0 = (int32_t)floorf(cx - span);
  *x1 = (int32_t)ceilf(cx + span);
  if (*x0 < g->x0) *x0 = g->x0;
  if (*x1 > g->x1) *x1 = g->x1;
  return *x0 < *x1;
}

static void draw_whole(h2_fruit_slice_canvas_t* canvas,
                       h2_fruit_slice_kind_t kind, float cx, float cy, float r,
                       float angle, uint64_t time_ms) {
  const palette_t* p = &k_palette[kind];
  const float ca = cosf(angle), sa = sinf(angle);
  if (kind == H2_FRUIT_SLICE_LEMON) {
    float x, y;
    const uint16_t tip = pack(scaled(p->skin, 0.9f));
    local_point(cx, cy, r, ca, sa, 0.95f, 0.0f, &x, &y);
    disc(canvas, x, y, 0.2f * r, tip, 256u);
    local_point(cx, cy, r, ca, sa, -0.95f, 0.0f, &x, &y);
    disc(canvas, x, y, 0.2f * r, tip, 256u);
  }

  region_t g;
  if (region_begin(canvas, cx - r - 1.0f, cy - r - 1.0f, cx + r + 1.0f,
                   cy + r + 1.0f, &g)) {
    const sprite_t* s = sprite_get(canvas->sprites, kind, false, r);
    const uint16_t flat = pack(p->skin);
    const float scale = s != NULL ? s->radius / r : 0.0f;
    const float outer = (r + 0.5f) * (r + 0.5f);
    const float inner = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : 0.0f;
    const float inverse = 1.0f / r, inverse2 = inverse * inverse;
    const float strength = kind == H2_FRUIT_SLICE_BOMB ? 0.35f : 0.55f;
    for (int32_t py = g.y0; py < g.y1; ++py) {
      const float dy = (float)py + 0.5f - cy, dy2 = dy * dy;
      int32_t x0, x1;
      if (!disc_span(&g, cx, dy2, outer, &x0, &x1)) continue;
      uint16_t* row = row_at(canvas, py);
      const float v = dy * inverse;
      const float light_row = 1.0f - 0.176f * v;
      const float hy = v + 0.40f, hy2 = hy * hy;
      for (int32_t px = x0; px < x1; ++px) {
        const float dx = (float)px + 0.5f - cx;
        const float d2 = dx * dx + dy2;
        if (d2 >= outer) continue;
        uint16_t albedo = flat;
        if (s != NULL) {
          const float lx = (dx * ca + dy * sa) * scale;
          const float ly = (dy * ca - dx * sa) * scale;
          albedo = s->texels[(size_t)texel_index(ly, s->extent, s->height) *
                                 (size_t)s->width +
                             (size_t)texel_index(lx, s->extent, s->width)];
        }
        const float u = dx * inverse, nd2 = d2 * inverse2;
        float light = light_row - 0.132f * u - 0.28f * nd2;
        if (nd2 > 0.8464f) light *= 0.62f;
        const float hx = u + 0.36f, spot = hx * hx + hy2;
        const float highlight =
            spot < 0.055f ? (1.0f - spot * (1.0f / 0.055f)) * strength : 0.0f;
        const uint16_t color =
            shade(albedo, light > 0.0f ? (uint32_t)(light * 256.0f) : 0u,
                  (uint32_t)(highlight * 256.0f));
        const uint32_t alpha =
            d2 <= inner
                ? 256u
                : (uint32_t)(clampf(r + 0.5f - sqrtf(d2), 0.0f, 1.0f) * 256.0f);
        row[px] = blend(row[px], color, alpha);
      }
    }
  }
  draw_whole_details(canvas, kind, cx, cy, r, ca, sa, time_ms);
}

static void draw_half(h2_fruit_slice_canvas_t* canvas,
                      h2_fruit_slice_kind_t kind, float cx, float cy, float r,
                      float angle) {
  const float nx = cosf(angle), ny = sinf(angle);
  region_t g;
  if (!region_begin(canvas, cx - r - 1.0f, cy - r - 1.0f, cx + r + 1.0f,
                    cy + r + 1.0f, &g))
    return;
  const sprite_t* s = sprite_get(canvas->sprites, kind, true, r);
  const uint16_t flat = pack(k_palette[kind].flesh);
  const float scale = s != NULL ? s->radius / r : 0.0f;
  const float outer = (r + 0.5f) * (r + 0.5f);
  const float inner = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : 0.0f;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    const float dy = (float)py + 0.5f - cy, dy2 = dy * dy;
    int32_t x0, x1;
    if (!disc_span(&g, cx, dy2, outer, &x0, &x1)) continue;
    uint16_t* row = row_at(canvas, py);
    for (int32_t px = x0; px < x1; ++px) {
      const float dx = (float)px + 0.5f - cx;
      const float side = dx * nx + dy * ny;
      if (side < -0.5f) continue;
      const float d2 = dx * dx + dy2;
      if (d2 >= outer) continue;
      float cover =
          d2 <= inner ? 1.0f : clampf(r + 0.5f - sqrtf(d2), 0.0f, 1.0f);
      if (side < 0.5f) cover = fminf(cover, side + 0.5f);
      if (cover <= 0.0f) continue;
      uint16_t color = flat;
      if (s != NULL) {
        const float a = side * scale, b = (dy * nx - dx * ny) * scale;
        color = s->texels[(size_t)texel_index(b, s->extent, s->height) *
                              (size_t)s->width +
                          (size_t)texel_index(a, 0, s->width)];
      }
      row[px] = blend(row[px], color, (uint32_t)(cover * 256.0f));
    }
  }
}

static void draw_cross(h2_fruit_slice_canvas_t* canvas, float cx, float cy,
                       float size, float thickness, uint16_t color,
                       uint32_t alpha) {
  capsule(canvas, cx - size, cy - size, cx + size, cy + size, thickness, color,
          alpha);
  capsule(canvas, cx - size, cy + size, cx + size, cy - size, thickness, color,
          alpha);
}

static uint32_t fade(uint32_t age, uint32_t life, uint32_t peak) {
  if (age >= life || life == 0u) return 0u;
  return peak * (life - age) / life;
}

static void draw_splats(h2_fruit_slice_canvas_t* canvas,
                        const h2_fruit_slice_game_t* game) {
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_SPLATS; ++i) {
    const h2_fruit_slice_splat_t* s = &game->splats[i];
    if (!s->active) continue;
    const uint32_t alpha = fade(s->age_ms, s->life_ms, 150u);
    disc(canvas, s->x, s->y, s->radius * 0.5f, s->color, alpha);
    const uint32_t seed = hash32((uint32_t)(int32_t)s->x * 7919u ^
                                 (uint32_t)(int32_t)s->y * 104729u);
    for (uint32_t k = 0; k < 7u; ++k) {
      const uint32_t h = hash32(seed + k * 0x9E3779B9u);
      const float angle =
          (float)(h & 1023u) * (2.0f * H2_FRUIT_SLICE_PI / 1024.0f);
      const float reach =
          (0.55f + (float)((h >> 10) & 63u) / 160.0f) * s->radius;
      const float size =
          (0.08f + (float)((h >> 16) & 31u) / 310.0f) * s->radius;
      disc(canvas, s->x + cosf(angle) * reach, s->y + sinf(angle) * reach, size,
           s->color, alpha);
    }
  }
}

static void draw_trail(h2_fruit_slice_canvas_t* canvas,
                       const h2_fruit_slice_game_t* game) {
  if (game->trail_count < 2u) return;
  const float width = 7.0f * game->unit;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i + 1u < game->trail_count; ++i) {
      const h2_fruit_slice_trail_point_t* p0 = &game->trail[i];
      const h2_fruit_slice_trail_point_t* p1 = &game->trail[i + 1u];
      const uint64_t elapsed =
          game->time_ms > p1->time_ms ? game->time_ms - p1->time_ms : 0u;
      if (elapsed >= H2_FRUIT_SLICE_TRAIL_LIFE_MS) continue;
      const float life =
          1.0f - (float)elapsed / (float)H2_FRUIT_SLICE_TRAIL_LIFE_MS;
      const float taper = (float)(i + 1u) / (float)(game->trail_count - 1u);
      const float r = width * life * (0.35f + 0.65f * taper);
      if (pass == 0)
        capsule(canvas, p0->x, p0->y, p1->x, p1->y, r * 1.9f,
                H2_FRUIT_SLICE_RGB(110, 214, 255), 70u);
      else
        capsule(canvas, p0->x, p0->y, p1->x, p1->y, r,
                H2_FRUIT_SLICE_RGB(255, 255, 255), 235u);
    }
  }
}

static void draw_hud(h2_fruit_slice_canvas_t* canvas,
                     const h2_fruit_slice_game_t* game) {
  const float u = game->unit;
  char value[24];
  draw_half(canvas, H2_FRUIT_SLICE_WATERMELON, 30.0f * u, 42.0f * u, 20.0f * u,
            -H2_FRUIT_SLICE_PI * 0.5f);
  (void)snprintf(value, sizeof(value), "%u", (unsigned)game->score);
  text(canvas, value, (int32_t)(58.0f * u), (int32_t)(14.0f * u),
       text_scale(u, 4.0f), H2_FRUIT_SLICE_RGB(255, 222, 80), 256u);
  (void)snprintf(value, sizeof(value), "BEST %u", (unsigned)game->best);
  text(canvas, value, (int32_t)(16.0f * u), (int32_t)(54.0f * u),
       text_scale(u, 2.0f), H2_FRUIT_SLICE_RGB(240, 232, 214), 220u);

  const uint32_t lost = H2_FRUIT_SLICE_LIVES - game->lives;
  for (uint32_t i = 0; i < H2_FRUIT_SLICE_LIVES; ++i) {
    const float size = (9.0f + 2.0f * (float)i) * u;
    const float x = (float)canvas->width - (22.0f + 34.0f * (float)i) * u;
    const bool red = i < lost;
    draw_cross(
        canvas, x, 30.0f * u, size, 3.5f * u,
        red ? H2_FRUIT_SLICE_RGB(232, 44, 44) : H2_FRUIT_SLICE_RGB(86, 98, 120),
        red ? 256u : 170u);
  }
}

static void draw_title(h2_fruit_slice_canvas_t* canvas,
                       const h2_fruit_slice_game_t* game) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const float u = game->unit;
  const char* title = "FRUIT SLICE";
  const int32_t big = fit_scale(title, w * 0.9f, text_scale(u, 7.0f));
  text_centered(canvas, title, w * 0.5f, h * 0.13f, big,
                H2_FRUIT_SLICE_RGB(255, 204, 48), 256u);
  const int32_t small = text_scale(u, 2.5f);
  text_centered(canvas, "SWIPE THE MELON TO START", w * 0.5f,
                h * 0.13f + (float)(big * 9),
                fit_scale("SWIPE THE MELON TO START", w * 0.9f, small),
                H2_FRUIT_SLICE_RGB(245, 238, 222), 240u);
  if (game->best > 0u) {
    char value[24];
    (void)snprintf(value, sizeof(value), "BEST %u", (unsigned)game->best);
    text_centered(canvas, value, w * 0.5f, h * 0.88f, small,
                  H2_FRUIT_SLICE_RGB(255, 222, 80), 230u);
  }
}

static void draw_game_over(h2_fruit_slice_canvas_t* canvas,
                           const h2_fruit_slice_game_t* game) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const float u = game->unit;
  const float panel_w = fminf(w * 0.78f, 420.0f * u);
  rounded_rect(canvas, (w - panel_w) * 0.5f, h * 0.17f, (w + panel_w) * 0.5f,
               h * 0.53f, 18.0f * u, H2_FRUIT_SLICE_RGB(20, 14, 10), 170u);
  const int32_t big =
      fit_scale("GAME OVER", panel_w * 0.9f, text_scale(u, 6.0f));
  text_centered(canvas, "GAME OVER", w * 0.5f, h * 0.21f, big,
                H2_FRUIT_SLICE_RGB(255, 92, 72), 256u);
  char value[24];
  (void)snprintf(value, sizeof(value), "SCORE %u", (unsigned)game->score);
  const int32_t middle = text_scale(u, 3.0f);
  text_centered(canvas, value, w * 0.5f, h * 0.21f + (float)(big * 9), middle,
                H2_FRUIT_SLICE_RGB(255, 236, 200), 256u);
  if (game->new_best)
    (void)snprintf(value, sizeof(value), "NEW BEST!");
  else
    (void)snprintf(value, sizeof(value), "BEST %u", (unsigned)game->best);
  text_centered(canvas, value, w * 0.5f,
                h * 0.21f + (float)(big * 9 + middle * 10), text_scale(u, 2.5f),
                game->new_best ? H2_FRUIT_SLICE_RGB(255, 222, 80)
                               : H2_FRUIT_SLICE_RGB(220, 214, 200),
                240u);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    const h2_fruit_slice_fruit_t* fruit = &game->fruits[i];
    if (!fruit->active || !fruit->menu) continue;
    text_centered(
        canvas, "SLICE TO PLAY AGAIN", w * 0.5f,
        fruit->body.y + fruit->radius + 10.0f * u,
        fit_scale("SLICE TO PLAY AGAIN", w * 0.9f, text_scale(u, 2.0f)),
        H2_FRUIT_SLICE_RGB(245, 238, 222), 220u);
  }
}

static const h2_fruit_slice_fruit_t* menu_fruit(
    const h2_fruit_slice_game_t* game) {
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i)
    if (game->fruits[i].active && game->fruits[i].menu) return &game->fruits[i];
  return NULL;
}

void h2_fruit_slice_static_key(const h2_fruit_slice_game_t* game,
                               h2_fruit_slice_static_key_t* out) {
  memset(out, 0, sizeof(*out));
  const h2_fruit_slice_fruit_t* menu = menu_fruit(game);
  out->phase = (uint32_t)game->phase;
  out->best = game->best;
  out->menu_y = menu != NULL ? (int32_t)menu->body.y : -1;
  if (game->phase != H2_FRUIT_SLICE_PHASE_TITLE) {
    out->score = game->score;
    out->lives = game->lives;
    out->new_best = game->new_best ? 1u : 0u;
  }
}

void h2_fruit_slice_render_static(h2_fruit_slice_canvas_t* canvas,
                                  const h2_fruit_slice_game_t* game) {
  if (game->phase == H2_FRUIT_SLICE_PHASE_TITLE) {
    draw_title(canvas, game);
    return;
  }
  if (game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER)
    draw_game_over(canvas, game);
  draw_hud(canvas, game);
}

void h2_fruit_slice_render_scene(h2_fruit_slice_canvas_t* canvas,
                                 const h2_fruit_slice_game_t* game) {
  const float u = game->unit;
  draw_splats(canvas, game);

  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_MISSES; ++i) {
    const h2_fruit_slice_miss_t* miss = &game->misses[i];
    if (!miss->active) continue;
    draw_cross(canvas, miss->x, (float)canvas->height - 34.0f * u, 14.0f * u,
               4.0f * u, H2_FRUIT_SLICE_RGB(236, 40, 40),
               fade(miss->age_ms, 1200u, 512u) > 256u
                   ? 256u
                   : fade(miss->age_ms, 1200u, 512u));
  }

  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_HALVES; ++i) {
    const h2_fruit_slice_half_t* half = &game->halves[i];
    if (half->active)
      draw_half(canvas, (h2_fruit_slice_kind_t)half->kind, half->body.x,
                half->body.y, half->radius, half->body.angle);
  }

  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i) {
    const h2_fruit_slice_fruit_t* fruit = &game->fruits[i];
    if (fruit->active)
      draw_whole(canvas, (h2_fruit_slice_kind_t)fruit->kind, fruit->body.x,
                 fruit->body.y, fruit->radius, fruit->body.angle,
                 game->time_ms);
  }

  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_PARTICLES; ++i) {
    const h2_fruit_slice_particle_t* p = &game->particles[i];
    if (p->active)
      disc(canvas, p->x, p->y, p->size, p->color,
           fade(p->age_ms, p->life_ms, 256u));
  }

  draw_trail(canvas, game);

  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_POPUPS; ++i) {
    const h2_fruit_slice_popup_t* popup = &game->popups[i];
    if (!popup->active) continue;
    const uint32_t alpha = fade(popup->age_ms, popup->life_ms, 512u);
    text_centered(canvas, popup->text, popup->x, popup->y, text_scale(u, 3.0f),
                  popup->color, alpha > 256u ? 256u : alpha);
  }

  if (game->flash_ms > 0u) {
    const uint32_t alpha = fade(H2_FRUIT_SLICE_FLASH_MS - game->flash_ms,
                                H2_FRUIT_SLICE_FLASH_MS, 220u);
    const float progress = (float)(H2_FRUIT_SLICE_FLASH_MS - game->flash_ms) /
                           (float)H2_FRUIT_SLICE_FLASH_MS;
    /* Keep the impact around the bomb. A full-screen alpha flash forces
     * every pixel through software blending and the display bus each frame. */
    const float radius = (36.0f + 60.0f * progress) * u;
    disc(canvas, game->flash_x, game->flash_y, radius,
         H2_FRUIT_SLICE_RGB(255, 248, 228), alpha);
  }
}
