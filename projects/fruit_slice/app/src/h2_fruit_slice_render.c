#include "h2_fruit_slice_render.h"

#include "h2_fruit_slice_cards_render.h"
#include "h2_fruit_slice_font.h"
#include "h2_fruit_slice_shape.h"

#define TR(value) h2_fruit_slice_translate(game->language, (value))

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
    [H2_FRUIT_SLICE_STRAWBERRY] = {{234, 38, 65},
                                   {255, 85, 104},
                                   {255, 224, 208},
                                   {255, 139, 163},
                                   {255, 204, 210},
                                   0.07f,
                                   0.0f},
    [H2_FRUIT_SLICE_BANANA] = {{255, 211, 32},
                               {255, 242, 102},
                               {255, 232, 112},
                               {255, 240, 199},
                               {255, 250, 226},
                               0.09f,
                               0.0f},
    [H2_FRUIT_SLICE_PINEAPPLE] = {{198, 133, 27},
                                  {250, 190, 44},
                                  {255, 230, 142},
                                  {255, 202, 52},
                                  {255, 239, 160},
                                  0.07f,
                                  0.0f},
    [H2_FRUIT_SLICE_GRAPES] = {{116, 43, 169},
                               {170, 79, 211},
                               {224, 172, 248},
                               {199, 137, 227},
                               {230, 189, 246},
                               0.09f,
                               0.0f},
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
static const uint8_t k_equal[7] = {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00};
static const uint8_t k_colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
static const uint8_t k_bang[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
static const uint8_t k_dot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
static const uint8_t k_times[7] = {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11};
static const uint8_t k_question[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
static const uint8_t k_slash[7] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};

static const uint8_t* glyph(char c) {
  if (c >= '0' && c <= '9') return k_digits[c - '0'];
  if (c >= 'A' && c <= 'Z') return k_letters[c - 'A'];
  if (c >= 'a' && c <= 'z') return c == 'x' ? k_times : k_letters[c - 'a'];
  switch (c) {
    case '+':
      return k_plus;
    case '-':
      return k_minus;
    case '=':
      return k_equal;
    case ':':
      return k_colon;
    case '!':
      return k_bang;
    case '.':
      return k_dot;
    case '?':
      return k_question;
    case '/':
      return k_slash;
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
    /* Most card/panel pixels are in straight spans. Their coverage is
     * exactly one; reserve float distance work for the rounded corners. */
    int32_t middle0 = g.x0, middle1 = g.x1;
    if (dy > 0.0f) {
      middle0 = (int32_t)ceilf(x0 + radius - 0.5f);
      middle1 = (int32_t)floorf(x1 - radius - 0.5f) + 1;
    } else {
      middle0 = (int32_t)ceilf(x0 - 0.5f);
      middle1 = (int32_t)floorf(x1 - 0.5f) + 1;
    }
    if (dy > radius) middle1 = middle0;
    if (middle0 < g.x0) middle0 = g.x0;
    if (middle0 > g.x1) middle0 = g.x1;
    if (middle1 < middle0) middle1 = middle0;
    if (middle1 > g.x1) middle1 = g.x1;
    for (int32_t px = middle0; px < middle1; ++px)
      row[px] = blend(row[px], color, alpha);
    for (int32_t px = g.x0; px < g.x1; ++px) {
      if (px == middle0 && middle1 > middle0) {
        px = middle1 - 1;
        continue;
      }
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

int32_t h2_fruit_slice_text_height(const char* value, int32_t scale) {
  if (value == NULL || *value == '\0' || scale <= 0) return 0;
  for (uint32_t cp; (cp = h2_fruit_slice_utf8_next(&value)) != 0u;)
    if (cp > 127u) return scale * 7 > 12 ? scale * 7 : 12;
  return scale * 7;
}

static int32_t text_advance(uint32_t cp, int32_t scale) {
  if (cp < 128u) return 6 * scale;
  return (scale * 7 > 12 ? scale * 7 : 12) + scale;
}

int32_t h2_fruit_slice_text_width(const char* value, int32_t scale) {
  if (value == NULL || *value == '\0' || scale <= 0) return 0;
  int32_t width = 0;
  for (uint32_t cp; (cp = h2_fruit_slice_utf8_next(&value)) != 0u;)
    width += text_advance(cp, scale);
  return width - scale;
}

/* Area sampling preserves one-pixel strokes when the 15px ink envelope is
 * reduced for a compact button. Coordinates use integer overlap weights. */
static uint32_t font_coverage(const lv_font_fmt_txt_glyph_dsc_t* g,
                              const uint8_t* bits, int32_t dx, int32_t dy,
                              int32_t cell) {
  const int32_t x0 = dx * 15, x1 = x0 + 15;
  const int32_t y0 = dy * 15, y1 = y0 + 15;
  const int32_t top = 13 - (int32_t)g->box_h - g->ofs_y;
  uint32_t sum = 0;
  for (int32_t y = y0 / cell; y * cell < y1; ++y) {
    const int32_t sy = y - top;
    if (sy < 0 || sy >= g->box_h) continue;
    const int32_t wy = (y1 < (y + 1) * cell ? y1 : (y + 1) * cell) -
                       (y0 > y * cell ? y0 : y * cell);
    for (int32_t x = x0 / cell; x * cell < x1; ++x) {
      const int32_t sx = x - g->ofs_x;
      if (sx < 0 || sx >= g->box_w) continue;
      const int32_t wx = (x1 < (x + 1) * cell ? x1 : (x + 1) * cell) -
                         (x0 > x * cell ? x0 : x * cell);
      const uint32_t index = (uint32_t)sy * g->box_w + (uint32_t)sx;
      const uint32_t a = (bits[index / 4u] >> (6u - 2u * (index % 4u))) & 3u;
      sum += a * (uint32_t)(wx * wy);
    }
  }
  return sum;
}

static void text_layer(h2_fruit_slice_canvas_t* canvas, const char* value,
                       int32_t x, int32_t y, int32_t scale, uint16_t color,
                       uint32_t alpha) {
  if (value == NULL || scale <= 0) return;
  const int32_t height = h2_fruit_slice_text_height(value, scale);
  for (uint32_t cp; (cp = h2_fruit_slice_utf8_next(&value)) != 0u;
       x += text_advance(cp, scale)) {
    if (cp < 128u) {
      const uint8_t* rows = glyph((char)cp);
      if (rows == NULL && cp != ' ') rows = k_question;
      if (rows == NULL) continue;
      for (int32_t row = 0; row < 7; ++row)
        for (int32_t column = 0; column < 5; ++column)
          if (rows[row] & (0x10u >> column))
            fill_rect(canvas, x + column * scale,
                      y + (height - 7 * scale) / 2 + row * scale, scale, scale,
                      color, alpha);
      continue;
    }
    const lv_font_fmt_txt_glyph_dsc_t* g = h2_fruit_slice_font_glyph(cp);
    if (g == NULL) {
      /* Missing/malformed input is visible and occupies the measured cell. */
      for (int32_t row = 0; row < 7; ++row)
        for (int32_t col = 0; col < 5; ++col)
          if (k_question[row] & (0x10u >> col))
            fill_rect(canvas, x + col * scale, y + row * scale, scale, scale,
                      color, alpha);
      continue;
    }
    const int32_t cell = text_advance(cp, scale) - scale;
    const lv_font_fmt_txt_dsc_t* dsc = h2_fruit_slice_font_asset.font->dsc;
    const uint8_t* bits = dsc->glyph_bitmap + g->bitmap_index;
    region_t region;
    if (!region_begin(canvas, (float)x, (float)y, (float)(x + cell),
                      (float)(y + height), &region))
      continue;
    for (int32_t py = region.y0; py < region.y1; ++py) {
      uint16_t* row = row_at(canvas, py);
      for (int32_t px = region.x0; px < region.x1; ++px) {
        const uint32_t coverage = font_coverage(g, bits, px - x, py - y, cell);
        if (coverage != 0u)
          row[px] = blend(row[px], color, coverage * alpha / (3u * 15u * 15u));
      }
    }
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

/* New silhouettes use zero as a transparent texel. All visible colors have
 * nonzero channels. Whole and full cut-face masks are prepared once; halves
 * sample the same fruit-local mask and clip against their moving cut normal. */
static uint16_t shaped_texel(h2_fruit_slice_kind_t kind, float x, float y,
                             bool half) {
  const palette_t* p = &k_palette[kind];
  const float body = h2_fruit_slice_shape_body(kind, x, y);
  const float leaf = h2_fruit_slice_shape_leaves(kind, x, y);
  if (body > 0.0f && (half || leaf > 0.0f)) return 0u;
  if (!half && leaf < 0.0f && y < -0.40f)
    return pack(scaled((color3_t){42, 124, 58}, leaf > -0.035f ? 0.55f : 1.0f));
  color3_t color = half ? p->flesh : p->skin;
  float edge = -body;
  if (kind == H2_FRUIT_SLICE_GRAPES) {
    unsigned count;
    const h2_fruit_slice_shape_circle_t* circles =
        h2_fruit_slice_shape_circles(kind, &count);
    /* Front berries overlap the back pair, but remain a single silhouette. */
    for (unsigned i = 0; i < count; ++i) {
      const float dx = x - circles[i].x, dy = y - circles[i].y;
      const float d = sqrtf(dx * dx + dy * dy);
      if (d > circles[i].radius) continue;
      edge = circles[i].radius - d;
      color = half ? p->flesh : mix(p->skin, p->skin_alt, 0.22f * (float)i);
      color = scaled(color, 0.92f - 0.20f * dx / circles[i].radius -
                                0.18f * dy / circles[i].radius);
    }
  }
  if (half) {
    if (edge < 0.035f)
      color = scaled(p->skin, 0.50f);
    else if (edge < p->rind)
      color = p->skin;
    else {
      if (kind == H2_FRUIT_SLICE_STRAWBERRY && fabsf(x) < 0.19f * (0.9f - y))
        color = p->flesh_alt;
      if (kind == H2_FRUIT_SLICE_PINEAPPLE && fabsf(x) < 0.14f)
        color = p->flesh_alt;
      color = scaled(color, 1.0f - 0.08f * (y + 1.0f));
    }
  } else {
    if (kind == H2_FRUIT_SLICE_STRAWBERRY) {
      static const seed_point_t seeds[] = {{-0.37f, -0.27f}, {0.24f, -0.29f},
                                           {-0.18f, 0.08f},  {0.37f, 0.12f},
                                           {0.04f, 0.43f},   {-0.08f, 0.65f}};
      for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        const float dx = x - seeds[i].a, dy = (y - seeds[i].b) * 0.65f;
        if (dx * dx + dy * dy < 0.0020f) color = (color3_t){255, 225, 145};
      }
    } else if (kind == H2_FRUIT_SLICE_BANANA) {
      if (fabsf(x) > 0.76f && y < -0.14f)
        color = (color3_t){109, 65, 25};
      else if (edge > 0.09f && edge < 0.16f)
        color = p->skin_alt;
    } else if (kind == H2_FRUIT_SLICE_PINEAPPLE) {
      const float a = (x + y) * 4.5f, b = (x - y) * 4.5f;
      if (fabsf(a - roundf(a)) < 0.10f || fabsf(b - roundf(b)) < 0.10f)
        color = (color3_t){130, 88, 24};
      else
        color = mix(p->skin, p->skin_alt, 0.65f);
    }
    if (edge < 0.035f) color = scaled(color, 0.52f);
  }
  return pack(color);
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
  const bool shaped = kind >= H2_FRUIT_SLICE_STRAWBERRY;
  const int32_t width = half && !shaped ? extent + 1 : 2 * extent + 1;
  const int32_t height = 2 * extent + 1;
  uint16_t* texels = h2_pal_mem_alloc(
      sprites->allocator, (size_t)width * (size_t)height * sizeof(uint16_t));
  if (texels == NULL) return NULL;
  for (int32_t ty = 0; ty < height; ++ty)
    for (int32_t tx = 0; tx < width; ++tx) {
      const float ly = (float)(ty - extent);
      if (shaped) {
        texels[(size_t)ty * (size_t)width + (size_t)tx] =
            shaped_texel(kind, (float)(tx - extent) / r, ly / r, half);
        continue;
      }
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
  return ok &&
         sprite_get(sprites, H2_FRUIT_SLICE_WATERMELON, true,
                    20.0f * game->unit) != NULL &&
         h2_fruit_slice_cards_prepare_sprites(sprites, game);
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
        if (kind >= H2_FRUIT_SLICE_STRAWBERRY) {
          if (s == NULL)
            albedo = shaped_texel(kind, (dx * ca + dy * sa) / r,
                                  (dy * ca - dx * sa) / r, false);
          if (albedo == 0u) continue;
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
                      float angle, float shape_angle) {
  const float nx = cosf(angle), ny = sinf(angle);
  const bool shaped = kind >= H2_FRUIT_SLICE_STRAWBERRY;
  const float ca = cosf(angle + shape_angle), sa = sinf(angle + shape_angle);
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
      if (shaped) {
        const float lx = dx * ca + dy * sa, ly = dy * ca - dx * sa;
        color = s != NULL ? s->texels[(size_t)texel_index(ly * scale, s->extent,
                                                          s->height) *
                                          (size_t)s->width +
                                      (size_t)texel_index(lx * scale, s->extent,
                                                          s->width)]
                          : shaped_texel(kind, lx / r, ly / r, true);
        if (color == 0u) continue;
        if (side < 2.0f) color = shade(color, 256u, 60u);
      } else if (s != NULL) {
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
  const h2_fruit_slice_weapon_t* weapon =
      h2_fruit_slice_weapon(game->progress.equipped);
  const float width = weapon->trail_width * game->unit;
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
        capsule(canvas, p0->x, p0->y, p1->x, p1->y, r * 1.9f, weapon->color,
                100u);
      else if (weapon->trail_style == 3u) {
        /* A bounded lightning kink follows each input segment. */
        const float dx = p1->x - p0->x, dy = p1->y - p0->y;
        const float length = fmaxf(1.0f, sqrtf(dx * dx + dy * dy));
        const float offset = r * (i % 2u ? -1.4f : 1.4f);
        const float mx = (p0->x + p1->x) * 0.5f - dy / length * offset;
        const float my = (p0->y + p1->y) * 0.5f + dx / length * offset;
        capsule(canvas, p0->x, p0->y, mx, my, r * 0.45f,
                H2_FRUIT_SLICE_RGB(255, 255, 240), 245u);
        capsule(canvas, mx, my, p1->x, p1->y, r * 0.45f,
                H2_FRUIT_SLICE_RGB(255, 255, 240), 245u);
      } else {
        capsule(canvas, p0->x, p0->y, p1->x, p1->y, r,
                H2_FRUIT_SLICE_RGB(255, 255, 255), 235u);
        if (weapon->trail_style == 1u)
          capsule(canvas, p0->x, p0->y, p1->x, p1->y, r * 0.35f,
                  H2_FRUIT_SLICE_RGB(76, 110, 38), 235u);
        if (weapon->trail_style == 2u && i % 2u == 0u)
          draw_cross(canvas, p1->x, p1->y, r * 1.6f, fmaxf(0.5f, r * 0.25f),
                     weapon->color, 200u);
      }
    }
  }
}

static void draw_hud(h2_fruit_slice_canvas_t* canvas,
                     const h2_fruit_slice_game_t* game) {
  const float u = game->unit, w = (float)canvas->width;
  const float margin = fmaxf(8.0f, 12.0f * u);
  const int32_t small = text_scale(u, 1.5f);
  const float label_h = (float)h2_fruit_slice_text_height(TR("SCORE"), small);
  text(canvas, TR("SCORE"), (int32_t)margin, (int32_t)margin, small,
       H2_FRUIT_SLICE_RGB(240, 232, 214), 240u);
  char value[16];
  (void)snprintf(value, sizeof(value), "%u", (unsigned)game->score);
  text(canvas, value, (int32_t)margin, (int32_t)(margin + label_h + 4.0f),
       fit_scale(value, w * 0.40f, text_scale(u, 5.0f)),
       H2_FRUIT_SLICE_RGB(255, 222, 80), 256u);

  const float spacing = fmaxf(12.0f, 22.0f * u);
  const float radius = fmaxf(5.0f, 9.0f * u);
  const float right = w - margin;
  text(canvas, TR("LIVES"),
       (int32_t)(right - (float)h2_fruit_slice_text_width(TR("LIVES"), small)),
       (int32_t)margin, small, H2_FRUIT_SLICE_RGB(240, 232, 214), 240u);
  for (uint32_t i = 0; i < H2_FRUIT_SLICE_LIVES; ++i) {
    const float x = right - radius - spacing * (float)(2u - i);
    const float y = margin + label_h + 6.0f + radius;
    if (i < game->lives) {
      const uint16_t heart = H2_FRUIT_SLICE_RGB(255, 100, 80);
      disc(canvas, x - radius * 0.38f, y - radius * 0.30f, radius * 0.55f,
           heart, 256u);
      disc(canvas, x + radius * 0.38f, y - radius * 0.30f, radius * 0.55f,
           heart, 256u);
      capsule(canvas, x - radius * 0.52f, y, x, y + radius * 0.65f,
              radius * 0.35f, heart, 256u);
      capsule(canvas, x + radius * 0.52f, y, x, y + radius * 0.65f,
              radius * 0.35f, heart, 256u);
    } else
      draw_cross(canvas, x, y, radius, fmaxf(1.5f, 2.0f * u),
                 H2_FRUIT_SLICE_RGB(140, 128, 120), 256u);
  }
}

static void draw_button(h2_fruit_slice_canvas_t* canvas,
                        const h2_fruit_slice_game_t* game, uint32_t id,
                        const char* label, bool enabled);

static void draw_title(h2_fruit_slice_canvas_t* canvas,
                       const h2_fruit_slice_game_t* game) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const float u = game->unit;
  draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_LANGUAGE,
              game->language == H2_FRUIT_SLICE_LANGUAGE_EN ? "中文" : "EN",
              true);
  for (unsigned i = 0; i < 2u; ++i) {
    h2_fruit_slice_rect_t r;
    (void)h2_fruit_slice_game_button_rect(
        game, i == 0u ? H2_FRUIT_SLICE_BUTTON_SFX : H2_FRUIT_SLICE_BUTTON_MUSIC,
        &r);
    const bool muted = (game->audio_mute & (1u << i)) != 0u;
    const bool compact = canvas->width < 240;
    const char* label = i == 0u ? TR(muted ? (compact ? "SOUND OFF" : "SFX OFF")
                                           : (compact ? "SOUND ON" : "SFX ON"))
                                : TR(muted ? (compact ? "MUSIC OFF" : "BGM OFF")
                                           : (compact ? "MUSIC ON" : "BGM ON"));
    if (compact && game->language == H2_FRUIT_SLICE_LANGUAGE_EN)
      label = i == 0u ? (muted ? "FX-" : "FX+") : (muted ? "BG-" : "BG+");
    rounded_rect(canvas, r.x0, r.y0, r.x1, r.y1, 6.0f * u,
                 H2_FRUIT_SLICE_RGB(22, 18, 16), 230u);
    const int32_t scale =
        fit_scale(label, r.x1 - r.x0 - 6.0f, text_scale(u, 1.5f));
    text_centered(
        canvas, label, (r.x0 + r.x1) * 0.5f,
        (r.y0 + r.y1 - (float)h2_fruit_slice_text_height(label, scale)) * 0.5f,
        scale,
        muted ? H2_FRUIT_SLICE_RGB(180, 170, 160)
              : H2_FRUIT_SLICE_RGB(255, 222, 80),
        256u);
  }
  const char* title = TR("FRUIT SLICE");
  const int32_t big =
      fit_scale(title, w * 0.90f, text_scale(u, h < 280.0f ? 4.0f : 6.0f));
  const int32_t small = text_scale(u, 2.5f);
  const float title_h = (float)h2_fruit_slice_text_height(title, big);
  const float info_h = (float)h2_fruit_slice_text_height(TR("COINS %u"), small);
  const float title_y = h < 220.0f ? 40.0f : fmaxf(44.0f, h * 0.16f);
  h2_fruit_slice_rect_t start;
  (void)h2_fruit_slice_game_button_rect(game, H2_FRUIT_SLICE_BUTTON_START,
                                        &start);
  const bool show_title =
      h >= 220.0f ||
      title_y + title_h + 8.0f + 2.0f * info_h <= start.y0 - 6.0f;
  if (show_title)
    text_centered(canvas, title, w * 0.5f, title_y, big,
                  H2_FRUIT_SLICE_RGB(255, 204, 48), 256u);
  char value[32];
  const bool compact = h < 220.0f || w < 220.0f;
  float info_y = compact ? h * 0.32f : h * 0.29f;
  if (h < 220.0f)
    info_y = title_y + (show_title ? title_h + 4.0f : 0.0f);
  else if (!compact)
    for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_FRUITS; ++i)
      if (game->fruits[i].active && game->fruits[i].menu)
        info_y = fminf(info_y, game->fruits[i].body.y - game->fruits[i].radius -
                                   info_h - 6.0f);
  (void)snprintf(value, sizeof(value), TR("COINS %u"),
                 (unsigned)game->progress.coins);
  text_centered(canvas, value, w * (compact ? 0.5f : 0.25f), info_y,
                fit_scale(value, w * (compact ? 0.90f : 0.46f), small),
                H2_FRUIT_SLICE_RGB(255, 222, 80), 240u);
  (void)snprintf(value, sizeof(value), TR("BEST %u"), (unsigned)game->best);
  text_centered(
      canvas, value, w * (compact ? 0.5f : 0.75f),
      compact ? (h < 220.0f ? info_y + info_h + 4.0f : h * 0.39f) : info_y,
      fit_scale(value, w * (compact ? 0.90f : 0.46f), small),
      H2_FRUIT_SLICE_RGB(240, 232, 214), 240u);
}

/* Include the painted shadow and a clear row in translated line stacks. */
static int32_t line_advance(const char* value, int32_t scale) {
  return h2_fruit_slice_text_height(value, scale) +
         (scale / 2 > 0 ? scale / 2 : 1) + 1;
}

static float draw_game_over(h2_fruit_slice_canvas_t* canvas,
                            const h2_fruit_slice_game_t* game) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const float u = game->unit;
  const int32_t heading =
      fit_scale(TR("GAME OVER"), w * 0.9f, text_scale(u, 4.0f));
  const int32_t small = text_scale(u, 2.5f);
  const bool compact = h < 220.0f;
  const float heading_y = compact ? 8.0f : h * 0.10f;
  text_centered(canvas, TR("GAME OVER"), w * 0.5f, heading_y, heading,
                H2_FRUIT_SLICE_RGB(255, 110, 80), 256u);
  char value[32];
  (void)snprintf(value, sizeof(value), "%u", (unsigned)game->score);
  const int32_t score_scale = fit_scale(value, w * 0.86f, text_scale(u, 7.0f));
  const float score_y = compact ? 28.0f : h * 0.29f;
  const float score_h = (float)line_advance(value, score_scale) + 2.0f;
  const float label_y = fminf(
      h * 0.23f,
      score_y - (float)h2_fruit_slice_text_height(TR("SCORE"), small) - 4.0f);
  if (!compact && label_y >= heading_y +
                                 (float)h2_fruit_slice_text_height(
                                     TR("GAME OVER"), heading) +
                                 4.0f)
    text_centered(canvas, TR("SCORE"), w * 0.5f, label_y, small,
                  H2_FRUIT_SLICE_RGB(240, 232, 214), 240u);
  text_centered(canvas, value, w * 0.5f, score_y, score_scale,
                H2_FRUIT_SLICE_RGB(255, 222, 80), 256u);
  (void)snprintf(value, sizeof(value),
                 TR(game->new_best ? "NEW BEST %u" : "BEST %u"),
                 (unsigned)game->best);
  const int32_t best_scale = fit_scale(value, w * 0.9f, small);
  h2_fruit_slice_rect_t start;
  (void)h2_fruit_slice_game_button_rect(game, H2_FRUIT_SLICE_BUTTON_START,
                                        &start);
  const float best_y =
      compact ? score_y + score_h
              : fminf(h * 0.44f,
                      start.y0 -
                          (float)h2_fruit_slice_text_height(value, best_scale) -
                          4.0f);
  text_centered(canvas, value, w * 0.5f, best_y, best_scale,
                H2_FRUIT_SLICE_RGB(245, 238, 222), 256u);
  return best_y + (float)line_advance(value, best_scale) + 2.0f;
}

static void draw_button(h2_fruit_slice_canvas_t* canvas,
                        const h2_fruit_slice_game_t* game, uint32_t id,
                        const char* label, bool enabled) {
  h2_fruit_slice_rect_t r;
  if (!h2_fruit_slice_game_button_rect(game, id, &r)) return;
  const bool primary = id == H2_FRUIT_SLICE_BUTTON_START;
  const uint16_t color = primary ? H2_FRUIT_SLICE_RGB(255, 198, 48)
                                 : H2_FRUIT_SLICE_RGB(54, 36, 22);
  rounded_rect(canvas, r.x0, r.y0, r.x1, r.y1, 8.0f * game->unit,
               enabled ? color : H2_FRUIT_SLICE_RGB(38, 32, 28), 256u);
  const int32_t scale =
      fit_scale(label, (r.x1 - r.x0) * 0.90f,
                text_scale(game->unit, primary ? 3.0f : 2.5f));
  const float label_h = (float)h2_fruit_slice_text_height(label, scale);
  const int32_t hint_scale = text_scale(game->unit, 1.5f);
  const bool storage_hint = id == H2_FRUIT_SLICE_BUTTON_BACK &&
                            game->height < 280 && game->save_status != 0u &&
                            game->phase != H2_FRUIT_SLICE_PHASE_CARDS;
  const char* hint = storage_hint
                         ? TR(game->save_status == 1u   ? "SESSION ONLY"
                              : game->save_status == 2u ? "UNSAVED - RETRYING"
                                                        : "UNSAVED - PROTECTED")
                         : TR("TAP / SLICE");
  const float hint_gap = storage_hint ? 2.0f : 4.0f;
  const float hint_text_h = (float)h2_fruit_slice_text_height(hint, hint_scale);
  const bool show_hint =
      (primary || storage_hint) &&
      label_h + hint_text_h + hint_gap <=
          r.y1 - r.y0 - (storage_hint ? 4.0f : 6.0f) &&
      (float)h2_fruit_slice_text_width(hint, hint_scale) <= r.x1 - r.x0 - 8.0f;
  const float hint_h = show_hint ? hint_text_h + hint_gap : 0.0f;
  const float y = (r.y0 + r.y1 - label_h - hint_h) * 0.5f;
  text_centered(canvas, label, (r.x0 + r.x1) * 0.5f, y, scale,
                enabled ? (primary ? H2_FRUIT_SLICE_RGB(32, 22, 12)
                                   : H2_FRUIT_SLICE_RGB(255, 232, 184))
                        : H2_FRUIT_SLICE_RGB(162, 148, 124),
                256u);
  if (show_hint)
    text_centered(canvas, hint, (r.x0 + r.x1) * 0.5f, y + label_h + hint_gap,
                  hint_scale,
                  enabled ? (primary ? H2_FRUIT_SLICE_RGB(64, 40, 16)
                                     : H2_FRUIT_SLICE_RGB(255, 210, 120))
                          : H2_FRUIT_SLICE_RGB(162, 148, 124),
                  256u);
}

static void draw_wallet(h2_fruit_slice_canvas_t* canvas,
                        const h2_fruit_slice_game_t* game, float y) {
  char value[96];
  (void)snprintf(value, sizeof(value), TR("COINS %u"),
                 (unsigned)game->progress.coins);
  const bool result = game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER;
  text_centered(canvas, value, (float)canvas->width * (result ? 0.62f : 0.5f),
                y,
                fit_scale(value, (float)canvas->width * (result ? 0.70f : 0.9f),
                          text_scale(game->unit, 3.0f)),
                H2_FRUIT_SLICE_RGB(255, 222, 80), 256u);
}

static void draw_progress_line(h2_fruit_slice_canvas_t* canvas,
                               const h2_fruit_slice_game_t* game,
                               const char* value, h2_fruit_slice_rect_t row,
                               unsigned line, float preferred, uint16_t color) {
  const float inset = fmaxf(2.0f, 4.0f * game->unit);
  const float line_h = (row.y1 - row.y0 - 2.0f * inset) / 3.0f;
  int32_t scale = fit_scale(value, row.x1 - row.x0 - 2.0f * inset,
                            text_scale(game->unit, preferred));
  while (scale > 1 && (float)(h2_fruit_slice_text_height(value, scale) +
                              (scale / 2 > 0 ? scale / 2 : 1)) > line_h)
    --scale;
  text(canvas, value, (int32_t)(row.x0 + inset),
       (int32_t)(row.y0 + inset + line_h * (float)line), scale, color, 256u);
}

static void draw_progress_page(h2_fruit_slice_canvas_t* canvas,
                               const h2_fruit_slice_game_t* game) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const bool tasks = game->phase == H2_FRUIT_SLICE_PHASE_TASKS;
  const h2_fruit_slice_progress_t* p = &game->progress;
  h2_fruit_slice_progress_layout_t layout;
  h2_fruit_slice_game_progress_layout(game, &layout);
  text_centered(canvas, tasks ? TR("COIN TASKS") : TR("WEAPON SHOP"), w * 0.5f,
                h * 0.04f, text_scale(game->unit, 4.0f),
                H2_FRUIT_SLICE_RGB(255, 204, 48), 256u);
  draw_wallet(canvas, game, h * 0.14f);
  char page[24];
  (void)snprintf(page, sizeof(page), "%u/%u", (unsigned)layout.page + 1u,
                 (unsigned)layout.pages);
  text_centered(canvas, page, w * 0.5f, layout.pager.y0,
                text_scale(game->unit, 2.0f), H2_FRUIT_SLICE_RGB(240, 232, 214),
                256u);
  draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_PREVIOUS, TR("PREV"), true);
  draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_NEXT, TR("NEXT"), true);
  for (uint32_t row = 0; row < layout.count; ++row) {
    const unsigned id = layout.first + row;
    const h2_fruit_slice_rect_t r = layout.rows[row];
    const float inset = fmaxf(2.0f, 4.0f * game->unit);
    rounded_rect(canvas, r.x0, r.y0, r.x1, r.y1, 8.0f * game->unit,
                 H2_FRUIT_SLICE_RGB(20, 14, 10), 220u);
    const char* name = tasks ? TR(h2_fruit_slice_task(id)->name)
                             : TR(h2_fruit_slice_weapon(id)->name);
    draw_progress_line(canvas, game, name, r, 0u, 2.5f,
                       H2_FRUIT_SLICE_RGB(245, 238, 222));
    char detail[96], foot[96], action[48];
    const char* label;
    bool enabled;
    uint32_t button;
    if (tasks) {
      const h2_fruit_slice_task_t* task = h2_fruit_slice_task(id);
      const uint32_t value = h2_fruit_slice_game_task_progress(game, id);
      const bool claimed =
          id == H2_FRUIT_SLICE_TASK_SCORE && p->score_task == 2u;
      const bool earned = h2_fruit_slice_task_progress(p, id) >= task->target;
      enabled = !claimed && earned &&
                p->coins <= H2_FRUIT_SLICE_PROGRESS_LIMIT - task->reward;
      label = claimed   ? TR("DONE")
              : enabled ? TR("CLAIM")
              : earned  ? TR("FULL")
                        : TR("PLAY");
      (void)snprintf(detail, sizeof(detail), TR("%u/%u +%u COINS"),
                     (unsigned)(value < task->target ? value : task->target),
                     (unsigned)task->target, (unsigned)task->reward);
      if (id >= H2_FRUIT_SLICE_TASK_VARIETY) {
        const uint32_t credit =
            p->challenge_credit[id - H2_FRUIT_SLICE_TASK_VARIETY];
        if (credit != 0u)
          (void)snprintf(foot, sizeof(foot), TR("READY x%u"), (unsigned)credit);
        else
          (void)snprintf(foot, sizeof(foot), "%s",
                         (game->run_challenges_awarded &
                          (1u << (id - H2_FRUIT_SLICE_TASK_VARIETY)))
                             ? TR("NEXT RUN")
                             : TR("ONCE PER RUN"));
      } else
        (void)snprintf(foot, sizeof(foot), "%s",
                       task->repeatable ? TR("REPEATABLE") : TR("ONE-TIME"));
      button = H2_FRUIT_SLICE_BUTTON_CLAIM + id;
    } else {
      const h2_fruit_slice_weapon_t* weapon = h2_fruit_slice_weapon(id);
      const bool owned = (p->owned & (1u << id)) != 0u;
      enabled = owned || p->coins >= weapon->price;
      label = p->equipped == id ? TR("ACTIVE")
              : owned           ? TR("EQUIP")
              : enabled         ? TR("BUY")
                                : TR("LOCKED");
      if (!owned) {
        (void)snprintf(action, sizeof(action), enabled ? TR("BUY%u") : "%u",
                       (unsigned)weapon->price);
        label = action;
      }
      (void)snprintf(detail, sizeof(detail), "%s", TR(weapon->detail));
      (void)snprintf(foot, sizeof(foot), "%s", TR(weapon->handling));
      button = H2_FRUIT_SLICE_BUTTON_WEAPON + id;
    }
    draw_progress_line(canvas, game, detail, r, 1u, 2.0f,
                       H2_FRUIT_SLICE_RGB(255, 210, 120));
    draw_progress_line(canvas, game, foot, r, 2u, 2.0f,
                       H2_FRUIT_SLICE_RGB(220, 205, 180));
    draw_button(canvas, game, button, label, enabled);
    if (!tasks) {
      const h2_fruit_slice_weapon_t* weapon = h2_fruit_slice_weapon(id);
      const h2_fruit_slice_rect_t a = layout.actions[row];
      capsule(canvas, a.x0 + inset, a.y1 - inset - 2.0f * game->unit,
              a.x1 - inset, a.y1 - inset - 2.0f * game->unit,
              weapon->trail_width * game->unit * 0.18f, weapon->color, 256u);
    }
  }
  const char* hint = game->save_status == 1u   ? TR("SESSION ONLY - NO STORAGE")
                     : game->save_status == 2u ? TR("SAVE FAILED - RETRYING")
                     : game->save_status == 3u ? TR("SAVE UNREADABLE - REOPEN")
                     : tasks                   ? TR("COMBO = 3+ FRUIT/SWIPE")
                                               : TR("CUT/MIN PX - COMBO MS");
  text_centered(canvas, hint, w * 0.5f, layout.hint.y0,
                fit_scale(hint, layout.hint.x1 - layout.hint.x0,
                          text_scale(game->unit, 2.0f)),
                H2_FRUIT_SLICE_RGB(240, 232, 214), 256u);
  draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_BACK, TR("BACK"), true);
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
  out->progress_revision = game->progress_revision;
  out->save_status = game->save_status;
  out->audio_mute = game->audio_mute;
  out->language = (uint32_t)game->language;
  out->cards_revision = game->cards.revision;
  out->menu_y = menu != NULL ? (int32_t)menu->body.y : -1;
  if (game->phase != H2_FRUIT_SLICE_PHASE_TITLE) {
    out->score = game->score;
    out->lives = game->lives;
    out->new_best = game->new_best ? 1u : 0u;
  }
}

void h2_fruit_slice_render_static(h2_fruit_slice_canvas_t* canvas,
                                  const h2_fruit_slice_game_t* game) {
  if (game->phase == H2_FRUIT_SLICE_PHASE_CARDS) {
    h2_fruit_slice_cards_render_album_static(canvas, game);
    draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_BACK, TR("BACK"), true);
    return;
  }
  if (game->phase == H2_FRUIT_SLICE_PHASE_TASKS ||
      game->phase == H2_FRUIT_SLICE_PHASE_SHOP) {
    draw_progress_page(canvas, game);
    return;
  }
  if (game->phase == H2_FRUIT_SLICE_PHASE_TITLE) {
    draw_title(canvas, game);
  } else if (game->phase == H2_FRUIT_SLICE_PHASE_GAME_OVER) {
    const float next_y = draw_game_over(canvas, game);
    const float cards_y = fmaxf(
        next_y, game->height < 220 ? 0.0f : (float)canvas->height * 0.51f);
    h2_fruit_slice_cards_render_result(canvas, game, cards_y);
    if (game->height >= 220) {
      const float wallet_y = fmaxf(
          (float)canvas->height * 0.58f,
          cards_y + (float)h2_fruit_slice_cards_result_height(game) + 4.0f);
      h2_fruit_slice_rect_t start;
      (void)h2_fruit_slice_game_button_rect(game, H2_FRUIT_SLICE_BUTTON_START,
                                            &start);
      if (wallet_y + (float)line_advance(TR("COINS %u"),
                                         text_scale(game->unit, 3.0f)) <
          start.y0)
        draw_wallet(canvas, game, wallet_y);
    }
  } else {
    draw_hud(canvas, game);
    h2_fruit_slice_cards_render_badge(canvas, game);
  }
  if (game->phase != H2_FRUIT_SLICE_PHASE_PLAYING) {
    const bool title = game->phase == H2_FRUIT_SLICE_PHASE_TITLE;
    const bool ready_to_play =
        title || game->time_ms - game->phase_started_ms >=
                     H2_FRUIT_SLICE_RESTART_DELAY_MS;
    draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_START,
                TR(title ? "START GAME" : "PLAY AGAIN"), ready_to_play);
    bool ready = false;
    for (unsigned i = 0; i < H2_FRUIT_SLICE_TASKS; ++i)
      ready |= h2_fruit_slice_task_progress(&game->progress, i) >=
                   h2_fruit_slice_task(i)->target &&
               !(i == 2u && game->progress.score_task == 2u);
    draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_TASKS, TR("TASKS"), true);
    draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_CARDS, TR("CARDS"), true);
    draw_button(canvas, game, H2_FRUIT_SLICE_BUTTON_SHOP, TR("SHOP"), true);
    const uint32_t notices[] = {
        ready ? H2_FRUIT_SLICE_BUTTON_TASKS : 0u,
        game->cards.unseen != 0u ? H2_FRUIT_SLICE_BUTTON_CARDS : 0u};
    for (unsigned i = 0; i < 2u; ++i) {
      h2_fruit_slice_rect_t r;
      if (notices[i] != 0u &&
          h2_fruit_slice_game_button_rect(game, notices[i], &r))
        disc(canvas, r.x1 - fmaxf(6.0f, 9.0f * game->unit),
             r.y0 + fmaxf(6.0f, 9.0f * game->unit),
             fmaxf(2.0f, 3.0f * game->unit), H2_FRUIT_SLICE_RGB(255, 210, 64),
             256u);
    }
  }
  h2_fruit_slice_cards_render_reveal_static(canvas, game);
}

void h2_fruit_slice_render_scene(h2_fruit_slice_canvas_t* canvas,
                                 const h2_fruit_slice_game_t* game) {
  const float u = game->unit;
  if (game->phase == H2_FRUIT_SLICE_PHASE_CARDS) {
    h2_fruit_slice_cards_render_album_scene(canvas, game);
    return;
  }
  if (game->phase == H2_FRUIT_SLICE_PHASE_TASKS ||
      game->phase == H2_FRUIT_SLICE_PHASE_SHOP)
    return;
  if (h2_fruit_slice_cards_reveal_active(&game->cards)) {
    /* The dimmed reveal hides the field, so only the card animates. */
    h2_fruit_slice_cards_render_reveal_scene(canvas, game);
    return;
  }
  draw_splats(canvas, game);

  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_MISSES; ++i) {
    const h2_fruit_slice_miss_t* miss = &game->misses[i];
    if (!miss->active || game->phase != H2_FRUIT_SLICE_PHASE_PLAYING) continue;
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
                half->body.y, half->radius, half->body.angle,
                half->shape_angle);
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

  const h2_fruit_slice_popup_t* combo = NULL;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_MAX_POPUPS; ++i) {
    const h2_fruit_slice_popup_t* popup = &game->popups[i];
    if (popup->active && (combo == NULL || popup->age_ms < combo->age_ms))
      combo = popup;
  }
  if (combo != NULL && game->phase == H2_FRUIT_SLICE_PHASE_PLAYING) {
    const uint32_t alpha = fade(combo->age_ms, combo->life_ms, 512u);
    const int32_t scale = fit_scale(combo->text, (float)canvas->width - 16.0f,
                                    text_scale(u, 3.0f));
    /* Keep only the latest combo on its own line below the score. Drop
     * cards retain their labels and center HUD landing destination. */
    text(canvas, combo->text, (int32_t)fmaxf(8.0f, 12.0f * u),
         (int32_t)fmaxf(48.0f, 80.0f * u), scale, combo->color,
         alpha > 256u ? 256u : alpha);
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
  h2_fruit_slice_cards_render_play_scene(canvas, game);
}

bool h2_fruit_slice_canvas_region(h2_fruit_slice_canvas_t* canvas, float x0,
                                  float y0, float x1, float y1,
                                  h2_fruit_slice_region_t* out) {
  region_t g;
  const bool paint = region_begin(canvas, x0, y0, x1, y1, &g);
  if (!paint) return false;
  *out = (h2_fruit_slice_region_t){g.x0, g.y0, g.x1, g.y1};
  return true;
}

uint16_t* h2_fruit_slice_canvas_row(const h2_fruit_slice_canvas_t* canvas,
                                    int32_t y) {
  return row_at(canvas, y);
}

uint16_t h2_fruit_slice_blend(uint16_t dst, uint16_t src, uint32_t alpha) {
  return blend(dst, src, alpha);
}

void h2_fruit_slice_draw_disc(h2_fruit_slice_canvas_t* canvas, float cx,
                              float cy, float r, uint16_t color,
                              uint32_t alpha) {
  disc(canvas, cx, cy, r, color, alpha);
}

void h2_fruit_slice_draw_capsule(h2_fruit_slice_canvas_t* canvas, float x0,
                                 float y0, float x1, float y1, float r,
                                 uint16_t color, uint32_t alpha) {
  capsule(canvas, x0, y0, x1, y1, r, color, alpha);
}

void h2_fruit_slice_draw_rounded_rect(h2_fruit_slice_canvas_t* canvas, float x0,
                                      float y0, float x1, float y1,
                                      float radius, uint16_t color,
                                      uint32_t alpha) {
  rounded_rect(canvas, x0, y0, x1, y1, radius, color, alpha);
}

void h2_fruit_slice_draw_text(h2_fruit_slice_canvas_t* canvas,
                              const char* value, int32_t x, int32_t y,
                              int32_t scale, uint16_t color, uint32_t alpha) {
  text(canvas, value, x, y, scale, color, alpha);
}

void h2_fruit_slice_draw_text_centered(h2_fruit_slice_canvas_t* canvas,
                                       const char* value, float cx, float y,
                                       int32_t scale, uint16_t color,
                                       uint32_t alpha) {
  text_centered(canvas, value, cx, y, scale, color, alpha);
}

int32_t h2_fruit_slice_text_scale(float unit, float base) {
  return text_scale(unit, base);
}

int32_t h2_fruit_slice_fit_scale(const char* value, float max_width,
                                 int32_t preferred) {
  return fit_scale(value, max_width, preferred);
}

static bool sprite_cached(const h2_fruit_slice_sprites_t* sprites,
                          h2_fruit_slice_kind_t kind, float radius) {
  if (sprites == NULL || !(radius >= 1.0f && radius <= 512.0f)) return false;
  const uint32_t key = (uint32_t)(radius * 8.0f + 0.5f);
  for (size_t i = 0; i < sprites->count; ++i) {
    const sprite_t* s = &sprites->entries[i];
    if (s->kind == kind && !s->half && s->radius_eighths == key) return true;
  }
  return false;
}

void h2_fruit_slice_draw_fruit(h2_fruit_slice_canvas_t* canvas,
                               h2_fruit_slice_kind_t kind, float cx, float cy,
                               float r, float angle, uint64_t time_ms) {
  if ((unsigned)kind >= H2_FRUIT_SLICE_KIND_COUNT || !(r > 0.0f)) return;
  h2_fruit_slice_sprites_t* sprites = canvas->sprites;
  if (!sprite_cached(sprites, kind, r)) canvas->sprites = NULL;
  draw_whole(canvas, kind, cx, cy, r, angle, time_ms);
  canvas->sprites = sprites;
}

bool h2_fruit_slice_sprites_prepare_fruit(h2_fruit_slice_sprites_t* sprites,
                                          h2_fruit_slice_kind_t kind, float r) {
  return sprite_get(sprites, kind, false, r) != NULL;
}
