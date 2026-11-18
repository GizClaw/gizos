#include "h2_xianxia_rpg_render.h"

#include "h2_xianxia_rpg_font_format.h"
#include "h2_xianxia_rpg_story.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern const lv_font_t h2_xianxia_rpg_font_body;
extern const lv_font_t h2_xianxia_rpg_font_title;

#define RGB H2_XIANXIA_RPG_RGB
#define PI H2_XIANXIA_RPG_PI
#define BAND_ROWS H2_XIANXIA_RPG_BAND_ROWS
#define LINE_PX H2_XIANXIA_RPG_LINE_PX
#define COLS H2_XIANXIA_RPG_MAP_COLS
#define ROWS H2_XIANXIA_RPG_MAP_ROWS

#define C_INK RGB(34, 30, 36)
#define C_PANEL RGB(26, 22, 30)
#define C_GOLD RGB(232, 196, 110)
#define C_TEXT RGB(244, 238, 224)
#define C_DIM RGB(150, 144, 136)
#define C_SHADOW RGB(10, 8, 12)
#define C_HP RGB(212, 66, 58)
#define C_MP RGB(72, 142, 222)
#define C_NARRATION RGB(196, 214, 230)

typedef struct region {
  int32_t x0, y0, x1, y1; /* Half-open, clipped to the canvas. */
} region_t;

typedef struct color3 {
  float r, g, b;
} color3_t;

typedef h2_xianxia_rpg_canvas_t canvas_t;
typedef h2_xianxia_rpg_game_t game_t;

/* ---- Color and math --------------------------------------------------- */

static uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

static float hash_unit(uint32_t a, uint32_t b) {
  return (float)(hash32(a * 73856093u ^ b * 19349663u) & 1023u) / 1023.0f;
}

static float clampf(float value, float low, float high) {
  if (!(value >= low)) return low;
  return value > high ? high : value;
}

static color3_t c3(float r, float g, float b) { return (color3_t){r, g, b}; }

static color3_t mix(color3_t a, color3_t b, float t) {
  return (color3_t){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                    a.b + (b.b - a.b) * t};
}

static color3_t scaled(color3_t a, float k) {
  return (color3_t){a.r * k, a.g * k, a.b * k};
}

static uint16_t pack(color3_t c) {
  return RGB((unsigned)clampf(c.r, 0.0f, 255.0f),
             (unsigned)clampf(c.g, 0.0f, 255.0f),
             (unsigned)clampf(c.b, 0.0f, 255.0f));
}

/* Ordered dither of about half an RGB565 step, for smooth gradients. */
static uint16_t pack_dither(color3_t c, int32_t x, int32_t y) {
  static const uint8_t bayer[16] = {0, 8,  2, 10, 12, 4, 14, 6,
                                    3, 11, 1, 9,  15, 7, 13, 5};
  const float d = ((float)bayer[((y & 3) << 2) | (x & 3)] - 7.5f) * 0.5f;
  return pack((color3_t){c.r + d, c.g + d * 0.5f, c.b + d});
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

static uint32_t alpha_of(float cover, uint32_t alpha) {
  return (uint32_t)(clampf(cover, 0.0f, 1.0f) * (float)alpha);
}

uint64_t h2_xianxia_rpg_all_bands(int32_t height) {
  if (height <= 0) return 0;
  const int32_t bands = (height + BAND_ROWS - 1) / BAND_ROWS;
  if (bands >= H2_XIANXIA_RPG_MAX_BANDS) return UINT64_MAX;
  return ((uint64_t)1 << bands) - 1u;
}

/* ---- Canvas primitives ----------------------------------------------- */

/* Clip a float box, record its bands and report whether to paint it. */
static bool region_begin(canvas_t* canvas, float fx0, float fy0, float fx1,
                         float fy1, region_t* out) {
  float lx = 0.0f, ly = 0.0f;
  float hx = (float)canvas->width, hy = (float)canvas->height;
  if (canvas->clipped) {
    lx = fmaxf(lx, (float)canvas->clip_x0);
    ly = fmaxf(ly, (float)canvas->clip_y0);
    hx = fminf(hx, (float)canvas->clip_x1);
    hy = fminf(hy, (float)canvas->clip_y1);
  }
  if (!(hx > lx && hy > ly)) return false;
  const int32_t x0 = (int32_t)floorf(clampf(fx0, lx, hx));
  const int32_t y0 = (int32_t)floorf(clampf(fy0, ly, hy));
  const int32_t x1 = (int32_t)ceilf(clampf(fx1, lx, hx));
  const int32_t y1 = (int32_t)ceilf(clampf(fy1, ly, hy));
  if (x0 >= x1 || y0 >= y1) return false;
  for (int32_t band = y0 / BAND_ROWS;
       band <= (y1 - 1) / BAND_ROWS && band < H2_XIANXIA_RPG_MAX_BANDS;
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

static uint16_t* row_at(const canvas_t* canvas, int32_t y) {
  return canvas->pixels + (size_t)y * canvas->stride_pixels;
}

static void clip_to(canvas_t* canvas, int32_t x0, int32_t y0, int32_t x1,
                    int32_t y1) {
  canvas->clipped = true;
  canvas->clip_x0 = x0;
  canvas->clip_y0 = y0;
  canvas->clip_x1 = x1;
  canvas->clip_y1 = y1;
}

static void unclip(canvas_t* canvas) { canvas->clipped = false; }

static void fill_rect(canvas_t* canvas, float x, float y, float w, float h,
                      uint16_t color, uint32_t alpha) {
  region_t g;
  if (!region_begin(canvas, x, y, x + w, y + h, &g)) return;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    for (int32_t px = g.x0; px < g.x1; ++px)
      row[px] = blend(row[px], color, alpha);
  }
}

static void gradient_rect(canvas_t* canvas, float x, float y, float w, float h,
                          color3_t top, color3_t bottom) {
  region_t g;
  if (!region_begin(canvas, x, y, x + w, y + h, &g)) return;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    const float t = h > 0.0f ? clampf(((float)py - y) / h, 0.0f, 1.0f) : 0.0f;
    const color3_t color = mix(top, bottom, t);
    uint16_t* row = row_at(canvas, py);
    for (int32_t px = g.x0; px < g.x1; ++px)
      row[px] = pack_dither(color, px, py);
  }
}

static void disc(canvas_t* canvas, float cx, float cy, float r, uint16_t color,
                 uint32_t alpha) {
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
      row[px] = blend(row[px], color, alpha_of(cover, alpha));
    }
  }
}

/* Soft glow: alpha falls off linearly from the center. */
static void glow(canvas_t* canvas, float cx, float cy, float r, uint16_t color,
                 uint32_t alpha) {
  if (!(r > 0.0f) || alpha == 0u) return;
  region_t g;
  if (!region_begin(canvas, cx - r, cy - r, cx + r, cy + r, &g)) return;
  const float inverse = 1.0f / r;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    const float dy = (float)py + 0.5f - cy;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float dx = (float)px + 0.5f - cx;
      const float d = sqrtf(dx * dx + dy * dy) * inverse;
      if (d >= 1.0f) continue;
      const float k = 1.0f - d;
      row[px] = blend(row[px], color, alpha_of(k * k, alpha));
    }
  }
}

static void ellipse(canvas_t* canvas, float cx, float cy, float rx, float ry,
                    float angle, uint16_t color, uint32_t alpha) {
  if (!(rx > 0.0f && ry > 0.0f) || alpha == 0u) return;
  const float extent = fmaxf(rx, ry) + 1.0f;
  region_t g;
  if (!region_begin(canvas, cx - extent, cy - extent, cx + extent, cy + extent,
                    &g))
    return;
  const float ca = cosf(angle), sa = sinf(angle);
  const float small = fminf(rx, ry);
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    const float dy = (float)py + 0.5f - cy;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float dx = (float)px + 0.5f - cx;
      const float u = (dx * ca + dy * sa) / rx;
      const float v = (dy * ca - dx * sa) / ry;
      const float q = u * u + v * v;
      if (q >= 1.6f) continue;
      const float distance = (sqrtf(q) - 1.0f) * small;
      const float cover = 0.5f - distance;
      if (cover <= 0.0f) continue;
      row[px] = blend(row[px], color, alpha_of(cover, alpha));
    }
  }
}

static void capsule(canvas_t* canvas, float x0, float y0, float x1, float y1,
                    float r, uint16_t color, uint32_t alpha) {
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
      const float cover = d2 <= inner ? 1.0f : r + 0.5f - sqrtf(d2);
      row[px] = blend(row[px], color, alpha_of(cover, alpha));
    }
  }
}

/* Anti-aliased convex polygon with up to 8 vertices in either winding. */
static void convex(canvas_t* canvas, const float* xs, const float* ys,
                   int32_t n, uint16_t color, uint32_t alpha) {
  if (n < 3 || n > 8 || alpha == 0u) return;
  float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
  float area = 0.0f;
  for (int32_t i = 0; i < n; ++i) {
    const int32_t j = (i + 1) % n;
    minx = fminf(minx, xs[i]);
    maxx = fmaxf(maxx, xs[i]);
    miny = fminf(miny, ys[i]);
    maxy = fmaxf(maxy, ys[i]);
    area += xs[i] * ys[j] - xs[j] * ys[i];
  }
  if (fabsf(area) < 1e-3f) return;
  region_t g;
  if (!region_begin(canvas, minx - 1.0f, miny - 1.0f, maxx + 1.0f, maxy + 1.0f,
                    &g))
    return;
  float ex[8], ey[8], el[8];
  const float sign = area > 0.0f ? 1.0f : -1.0f;
  for (int32_t i = 0; i < n; ++i) {
    const int32_t j = (i + 1) % n;
    ex[i] = xs[j] - xs[i];
    ey[i] = ys[j] - ys[i];
    const float length = sqrtf(ex[i] * ex[i] + ey[i] * ey[i]);
    el[i] = length > 0.0f ? sign / length : 0.0f;
  }
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    const float fy = (float)py + 0.5f;
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float fx = (float)px + 0.5f;
      float nearest = 1e9f;
      for (int32_t i = 0; i < n; ++i) {
        const float d = (ex[i] * (fy - ys[i]) - ey[i] * (fx - xs[i])) * el[i];
        if (d < nearest) nearest = d;
      }
      const float cover = nearest + 0.5f;
      if (cover <= 0.0f) continue;
      row[px] = blend(row[px], color, alpha_of(cover, alpha));
    }
  }
}

static void triangle(canvas_t* canvas, float x0, float y0, float x1, float y1,
                     float x2, float y2, uint16_t color, uint32_t alpha) {
  const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
  convex(canvas, xs, ys, 3, color, alpha);
}

static void quad(canvas_t* canvas, float x0, float y0, float x1, float y1,
                 float x2, float y2, float x3, float y3, uint16_t color,
                 uint32_t alpha) {
  const float xs[4] = {x0, x1, x2, x3}, ys[4] = {y0, y1, y2, y3};
  convex(canvas, xs, ys, 4, color, alpha);
}

static void rounded_rect(canvas_t* canvas, float x0, float y0, float x1,
                         float y1, float radius, uint16_t color,
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
      if (cover > 0.0f) row[px] = blend(row[px], color, alpha_of(cover, alpha));
    }
  }
}

/* A dark panel with a thin gold rim. */
static void panel(canvas_t* canvas, float x, float y, float w, float h,
                  float radius, uint32_t alpha) {
  rounded_rect(canvas, x, y, x + w, y + h, radius, C_GOLD, alpha);
  rounded_rect(canvas, x + 2.0f, y + 2.0f, x + w - 2.0f, y + h - 2.0f,
               radius - 1.5f > 1.0f ? radius - 1.5f : 1.0f, C_PANEL, 256u);
  rounded_rect(canvas, x + 4.0f, y + 4.0f, x + w - 4.0f, y + h - 4.0f,
               radius - 3.0f > 1.0f ? radius - 3.0f : 1.0f, RGB(36, 30, 40),
               160u);
}

/* ---- Text -------------------------------------------------------------- */

static const lv_font_t* font_of(bool title) {
  return title ? &h2_xianxia_rpg_font_title : &h2_xianxia_rpg_font_body;
}

static const lv_font_fmt_txt_dsc_t* font_dsc(const lv_font_t* font) {
  return (const lv_font_fmt_txt_dsc_t*)font->dsc;
}

static uint32_t glyph_index(const lv_font_t* font, uint32_t codepoint) {
  const lv_font_fmt_txt_dsc_t* dsc = font_dsc(font);
  for (uint32_t i = 0; i < dsc->cmap_num; ++i) {
    const lv_font_fmt_txt_cmap_t* cmap = &dsc->cmaps[i];
    if (codepoint < cmap->range_start) continue;
    const uint32_t offset = codepoint - cmap->range_start;
    if (offset >= cmap->range_length) continue;
    switch (cmap->type) {
      case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
        return cmap->glyph_id_start + offset;
      case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
        return cmap->glyph_id_start +
               ((const uint8_t*)cmap->glyph_id_ofs_list)[offset];
      case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY:
      case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL: {
        uint32_t low = 0, high = cmap->list_length;
        while (low < high) {
          const uint32_t middle = (low + high) / 2u;
          const uint32_t value = cmap->unicode_list[middle];
          if (value == offset) {
            if (cmap->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY)
              return cmap->glyph_id_start + middle;
            return cmap->glyph_id_start +
                   ((const uint16_t*)cmap->glyph_id_ofs_list)[middle];
          }
          if (value < offset)
            low = middle + 1u;
          else
            high = middle;
        }
        break;
      }
    }
  }
  return 0;
}

bool h2_xianxia_rpg_has_glyph(bool title, uint32_t codepoint) {
  return glyph_index(font_of(title), codepoint) != 0u;
}

static int32_t advance(const lv_font_t* font, uint32_t codepoint) {
  const uint32_t id = glyph_index(font, codepoint);
  if (id == 0u) return font->line_height / 2;
  return (int32_t)((font_dsc(font)->glyph_dsc[id].adv_w + 8u) >> 4);
}

int32_t h2_xianxia_rpg_text_width(bool title, const char* text) {
  const lv_font_t* font = font_of(title);
  int32_t width = 0;
  if (text == NULL) return 0;
  for (uint32_t cp = h2_xianxia_rpg_utf8_next(&text); cp != 0u;
       cp = h2_xianxia_rpg_utf8_next(&text))
    width += advance(font, cp);
  return width;
}

/* Draw one glyph with its line top at y; returns the advance. */
static int32_t glyph(canvas_t* canvas, const lv_font_t* font,
                     uint32_t codepoint, int32_t x, int32_t y, uint16_t color,
                     uint32_t alpha) {
  const uint32_t id = glyph_index(font, codepoint);
  if (id == 0u) return font->line_height / 2;
  const lv_font_fmt_txt_dsc_t* dsc = font_dsc(font);
  const lv_font_fmt_txt_glyph_dsc_t* g = &dsc->glyph_dsc[id];
  const int32_t baseline = y + font->line_height - font->base_line;
  const int32_t left = x + g->ofs_x;
  const int32_t top = baseline - (int32_t)g->box_h - g->ofs_y;
  region_t r;
  if (g->box_w != 0u && g->box_h != 0u &&
      region_begin(canvas, (float)left, (float)top, (float)(left + g->box_w),
                   (float)(top + g->box_h), &r)) {
    const uint8_t* bits = dsc->glyph_bitmap + g->bitmap_index;
    for (int32_t py = r.y0; py < r.y1; ++py) {
      uint16_t* row = row_at(canvas, py);
      for (int32_t px = r.x0; px < r.x1; ++px) {
        const uint32_t index =
            (uint32_t)(py - top) * g->box_w + (uint32_t)(px - left);
        uint32_t value;
        if (dsc->bpp == 4u)
          value = (index & 1u) ? bits[index >> 1] & 0x0Fu
                               : (uint32_t)bits[index >> 1] >> 4;
        else
          value = (uint32_t)(bits[index >> 3] >> (7u - (index & 7u))) & 1u ? 15u
                                                                           : 0u;
        if (value == 0u) continue;
        row[px] = blend(row[px], color, (value * 17u + 1u) * alpha / 256u);
      }
    }
  }
  return (int32_t)((g->adv_w + 8u) >> 4);
}

/* One line of text with a soft shadow; (x, y) is the line top. */
static int32_t text_at(canvas_t* canvas, bool title, const char* text,
                       int32_t x, int32_t y, uint16_t color, uint32_t alpha) {
  const lv_font_t* font = font_of(title);
  const int32_t offset = title ? 2 : 1;
  int32_t pen = x;
  for (const char* s = text; *s != '\0';) {
    const uint32_t cp = h2_xianxia_rpg_utf8_next(&s);
    (void)glyph(canvas, font, cp, pen + offset, y + offset, C_SHADOW,
                alpha * 3u / 4u);
    pen += glyph(canvas, font, cp, pen, y, color, alpha);
  }
  return pen - x;
}

static void text_centered(canvas_t* canvas, bool title, const char* text,
                          float cx, int32_t y, uint16_t color, uint32_t alpha) {
  const int32_t width = h2_xianxia_rpg_text_width(title, text);
  (void)text_at(canvas, title, text, (int32_t)(cx - (float)width * 0.5f), y,
                color, alpha);
}

/* Text on light paper: a pale offset instead of a dark shadow. */
static void paper_centered(canvas_t* canvas, bool title, const char* text,
                           float cx, int32_t y, uint16_t color,
                           uint32_t alpha) {
  const lv_font_t* font = font_of(title);
  int32_t pen =
      (int32_t)(cx - (float)h2_xianxia_rpg_text_width(title, text) * 0.5f);
  for (const char* s = text; *s != '\0';) {
    const uint32_t cp = h2_xianxia_rpg_utf8_next(&s);
    (void)glyph(canvas, font, cp, pen + 1, y + 1, RGB(255, 252, 240),
                alpha / 2u);
    pen += glyph(canvas, font, cp, pen, y, color, alpha);
  }
}

static bool attaches_left(uint32_t cp) {
  switch (cp) {
    case 0xFF0Cu: /* fullwidth comma */
    case 0x3002u: /* ideographic full stop */
    case 0xFF01u: /* fullwidth exclamation */
    case 0xFF1Fu: /* fullwidth question */
    case 0x3001u: /* ideographic comma */
    case 0xFF1Bu: /* fullwidth semicolon */
    case 0xFF1Au: /* fullwidth colon */
    case 0x201Du: /* right double quotation mark */
    case 0x300Bu: /* right double angle bracket */
    case 0xFF09u: /* fullwidth right parenthesis */
    case 0x2026u: /* ellipsis */
    case ',':
    case '.':
    case '!':
    case '?':
      return true;
    default:
      return false;
  }
}

/* Wrap text into lines of at most max_w pixels, drawing at most max_chars
 * codepoints. Breaks keep ASCII runs together and never start a line with
 * closing punctuation. Returns the number of lines used. */
static int32_t wrapped_with(canvas_t* canvas, const char* text, int32_t x,
                            int32_t y, int32_t max_w, int32_t max_lines,
                            uint16_t color, uint32_t alpha, uint32_t max_chars,
                            uint16_t shadow);

static int32_t text_wrapped(canvas_t* canvas, const char* text, int32_t x,
                            int32_t y, int32_t max_w, int32_t max_lines,
                            uint16_t color, uint32_t alpha,
                            uint32_t max_chars) {
  return wrapped_with(canvas, text, x, y, max_w, max_lines, color, alpha,
                      max_chars, C_SHADOW);
}

static int32_t wrapped_with(canvas_t* canvas, const char* text, int32_t x,
                            int32_t y, int32_t max_w, int32_t max_lines,
                            uint16_t color, uint32_t alpha, uint32_t max_chars,
                            uint16_t shadow) {
  const lv_font_t* font = font_of(false);
  int32_t line = 0, pen = 0;
  uint32_t index = 0;
  const char* s = text;
  while (*s != '\0') {
    const char* token = s;
    uint32_t first = h2_xianxia_rpg_utf8_next(&s);
    uint32_t chars = 1;
    int32_t width = advance(font, first);
    if (first < 0x80u && first != ' ') {
      while (*s != '\0' && (unsigned char)*s < 0x80u && *s != ' ') {
        width += advance(font, (unsigned char)*s);
        ++s;
        ++chars;
      }
    }
    if (pen > 0 && pen + width > max_w && !attaches_left(first)) {
      ++line;
      pen = 0;
      if (line >= max_lines) break;
      if (first == ' ') {
        index += chars;
        continue;
      }
    }
    for (uint32_t k = 0; k < chars; ++k) {
      const uint32_t cp = h2_xianxia_rpg_utf8_next(&token);
      const int32_t gx = x + pen, gy = y + line * LINE_PX;
      if (index < max_chars) {
        (void)glyph(canvas, font, cp, gx + 1, gy + 1, shadow, alpha * 3u / 4u);
        (void)glyph(canvas, font, cp, gx, gy, color, alpha);
      }
      pen += advance(font, cp);
      ++index;
    }
  }
  return line + 1;
}

/* ---- Shared widgets ---------------------------------------------------- */

static void bar(canvas_t* canvas, int32_t x, int32_t y, int32_t w, int32_t h,
                uint32_t value, uint32_t max, uint16_t color) {
  if (w < 8) return;
  const float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;
  rounded_rect(canvas, fx, fy, fx + fw, fy + fh, fh * 0.5f, RGB(12, 10, 14),
               230u);
  const float fill = max != 0u ? (fw - 4.0f) * (float)value / (float)max : 0.0f;
  if (fill > 1.0f) {
    rounded_rect(canvas, fx + 2.0f, fy + 2.0f, fx + 2.0f + fill, fy + fh - 2.0f,
                 (fh - 4.0f) * 0.5f, color, 256u);
    rounded_rect(canvas, fx + 3.0f, fy + 3.0f, fx + 1.0f + fill,
                 fy + fh * 0.45f, (fh - 4.0f) * 0.25f, RGB(255, 255, 255), 60u);
  }
  char numbers[24];
  (void)snprintf(numbers, sizeof(numbers), "%u/%u", (unsigned)value,
                 (unsigned)max);
  const int32_t tw = h2_xianxia_rpg_text_width(false, numbers);
  if (tw + 6 <= w)
    (void)text_at(canvas, false, numbers, x + (w - tw) / 2,
                  y + (h - font_of(false)->line_height) / 2, C_TEXT, 240u);
}

/* Label followed by a bar filling the rest of width. */
static void labeled_bar(canvas_t* canvas, const char* label, int32_t x,
                        int32_t y, int32_t w, uint32_t value, uint32_t max,
                        uint16_t color) {
  const int32_t lw = text_at(canvas, false, label, x, y, C_DIM, 240u) + 6;
  bar(canvas, x + lw, y + 2, w - lw, LINE_PX - 4, value, max, color);
}

static void diamond(canvas_t* canvas, float cx, float cy, float r,
                    uint16_t color, uint32_t alpha) {
  quad(canvas, cx, cy - r, cx + r * 0.7f, cy, cx, cy + r, cx - r * 0.7f, cy,
       color, alpha);
}

/* ---- Landscape painting (title, ending, battle) ------------------------ */

/* A ridge line height in [0, 1] from layered sines. */
static float ridge(float x, float seed) {
  return 0.5f + 0.25f * sinf(x * 2.1f + seed) +
         0.15f * sinf(x * 5.3f + seed * 1.7f) +
         0.07f * sinf(x * 13.1f + seed * 2.9f);
}

static void mountains(canvas_t* canvas, float top, float bottom, color3_t far,
                      color3_t near, color3_t haze, float seed) {
  region_t g;
  if (!region_begin(canvas, 0.0f, top, (float)canvas->width, bottom, &g))
    return;
  const float w = (float)canvas->width;
  for (int32_t layer = 0; layer < 3; ++layer) {
    const float k = (float)layer / 2.0f;
    const color3_t ink = mix(far, near, k);
    const float base = top + (bottom - top) * (0.15f + 0.28f * (float)layer);
    const float amplitude = (bottom - top) * (0.5f - 0.1f * (float)layer);
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const float x = (float)px / w;
      const float crest =
          base + amplitude * (1.0f - ridge(x * (1.0f + k), seed + 3.1f * k));
      int32_t y0 = (int32_t)crest;
      if (y0 < g.y0) y0 = g.y0;
      for (int32_t py = y0; py < g.y1; ++py) {
        /* Ink fades into mist below each crest. */
        const float depth =
            clampf(((float)py - crest) / (amplitude * 0.9f + 1.0f), 0.0f, 1.0f);
        const color3_t color = mix(ink, haze, depth * 0.85f);
        uint16_t* row = row_at(canvas, py);
        const uint32_t alpha = (float)py < crest + 1.0f
                                   ? alpha_of(crest + 1.0f - (float)py, 256u)
                                   : 256u;
        row[px] = blend(row[px], pack_dither(color, px, py), alpha);
      }
    }
  }
}

static void bamboo_stalk(canvas_t* canvas, float x, float y0, float y1,
                         float width, uint16_t color, uint32_t alpha) {
  fill_rect(canvas, x - width * 0.5f, y0, width, y1 - y0, color, alpha);
  const float segment = width * 7.0f;
  for (float y = y1 - segment; y > y0; y -= segment)
    fill_rect(canvas, x - width * 0.7f, y, width * 1.4f,
              fmaxf(1.0f, width * 0.25f), color, alpha);
}

static void bamboo_leaves(canvas_t* canvas, float x, float y, float size,
                          uint16_t color, uint32_t alpha, uint32_t seed) {
  for (uint32_t i = 0; i < 5u; ++i) {
    const float angle =
        -0.6f + 1.4f * hash_unit(seed, i) + (i % 2u ? PI : 0.0f);
    ellipse(canvas, x + cosf(angle) * size * 0.9f,
            y + sinf(angle) * size * 0.35f, size, size * 0.18f, angle, color,
            alpha);
  }
}

static void paper_landscape(canvas_t* canvas, const game_t* game, bool dawn,
                            bool river) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const float u = game->unit;
  gradient_rect(canvas, 0.0f, 0.0f, w, h,
                dawn ? c3(250, 226, 196) : c3(240, 232, 212),
                dawn ? c3(232, 214, 190) : c3(216, 206, 184));
  /* Paper grain. */
  region_t g;
  if (region_begin(canvas, 0.0f, 0.0f, w, h, &g)) {
    for (int32_t py = g.y0; py < g.y1; ++py) {
      uint16_t* row = row_at(canvas, py);
      for (int32_t px = g.x0; px < g.x1; ++px) {
        const uint32_t n =
            hash32((uint32_t)px * 2654435761u ^ (uint32_t)py * 40503u) & 31u;
        if (n < 3u) row[px] = blend(row[px], RGB(150, 130, 100), 40u);
      }
    }
  }
  disc(canvas, w * 0.74f, h * 0.2f, 34.0f * u,
       dawn ? RGB(232, 120, 80) : RGB(214, 88, 70), dawn ? 230u : 200u);
  mountains(canvas, h * 0.30f, h * 0.80f, c3(150, 160, 170), c3(60, 70, 82),
            c3(232, 224, 206), dawn ? 4.2f : 1.3f);
  /* River and mist bands. */
  gradient_rect(canvas, 0.0f, h * 0.80f, w, h * 0.20f, c3(206, 210, 204),
                c3(180, 192, 194));
  for (int32_t i = 0; i < 5; ++i) {
    const float y = h * (0.83f + 0.03f * (float)i);
    capsule(canvas, w * (0.1f + 0.13f * (float)i), y,
            w * (0.3f + 0.13f * (float)i), y, 1.2f * u, RGB(120, 140, 150),
            110u);
  }
  if (river) {
    /* The ferry crossing: a small boat with its lamp and lights upstream. */
    const float bx = w * 0.62f, by = h * 0.86f;
    quad(canvas, bx - 34.0f * u, by - 4.0f * u, bx + 40.0f * u, by - 7.0f * u,
         bx + 30.0f * u, by + 6.0f * u, bx - 26.0f * u, by + 6.0f * u,
         RGB(70, 56, 46), 230u);
    capsule(canvas, bx + 30.0f * u, by - 6.0f * u, bx + 30.0f * u,
            by - 30.0f * u, 1.2f * u, RGB(60, 48, 40), 230u);
    glow(canvas, bx + 30.0f * u, by - 30.0f * u, 14.0f * u, RGB(255, 200, 120),
         200u);
    capsule(canvas, bx - 6.0f * u, by - 6.0f * u, bx - 26.0f * u,
            by - 40.0f * u, 1.0f * u, RGB(90, 80, 60), 220u);
    for (int32_t i = 0; i < 6; ++i)
      glow(canvas, w * (0.12f + 0.08f * (float)i),
           h * (0.84f + 0.015f * (float)(i % 3)), 6.0f * u, RGB(255, 190, 110),
           170u);
  }
  /* Bamboo framing both edges. */
  const uint16_t dark = RGB(42, 70, 52), mid = RGB(70, 104, 72);
  for (int32_t i = 0; i < 4; ++i) {
    const float x = w * (0.02f + 0.035f * (float)i);
    bamboo_stalk(canvas, x, h * (0.05f * (float)i), h, (6.0f - (float)i) * u,
                 i % 2 ? mid : dark, 230u);
    bamboo_leaves(canvas, x + 10.0f * u, h * (0.18f + 0.12f * (float)i),
                  20.0f * u, dark, 200u, (uint32_t)i * 7u + 1u);
    const float rx = w * (0.98f - 0.04f * (float)i);
    bamboo_stalk(canvas, rx, h * (0.12f * (float)i), h, (5.0f - (float)i) * u,
                 i % 2 ? dark : mid, 220u);
    bamboo_leaves(canvas, rx - 12.0f * u, h * (0.26f + 0.1f * (float)i),
                  18.0f * u, mid, 200u, (uint32_t)i * 11u + 3u);
  }
}

/* Misty river shore for the dock and the reed flats; the hold of the old
 * ferry on the sandbar. */
static void river_backdrop(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float w = (float)canvas->width, sh = (float)l->stage.h;
  const float u = game->unit;
  if (game->map == H2_XIANXIA_RPG_MAP_WRECK) {
    gradient_rect(canvas, 0.0f, 0.0f, w, sh * 0.66f, c3(38, 46, 56),
                  c3(70, 78, 88));
    for (int32_t i = 0; i < 12; ++i)
      fill_rect(canvas, w * (float)i / 12.0f, 0.0f, 2.0f * u, sh * 0.66f,
                RGB(28, 32, 40), 200u);
    gradient_rect(canvas, 0.0f, sh * 0.66f, w, sh * 0.34f, c3(92, 76, 62),
                  c3(58, 48, 40));
    for (int32_t i = 0; i < 6; ++i)
      fill_rect(canvas, 0.0f, sh * (0.7f + 0.05f * (float)i), w, 1.0f,
                RGB(40, 32, 28), 220u);
    const bool clear = game->stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN;
    for (int32_t i = 0; i < 7; ++i)
      glow(canvas, w * hash_unit(41u, (uint32_t)i),
           sh * (0.3f + 0.5f * hash_unit(43u, (uint32_t)i)),
           (30.0f + 24.0f * hash_unit(47u, (uint32_t)i)) * u,
           clear ? RGB(255, 210, 130) : RGB(130, 100, 180), 80u);
  } else {
    gradient_rect(canvas, 0.0f, 0.0f, w, sh * 0.5f, c3(196, 206, 210),
                  c3(222, 226, 224));
    mountains(canvas, sh * 0.1f, sh * 0.5f, c3(170, 182, 186),
              c3(120, 136, 140), c3(220, 224, 224), 5.1f);
    gradient_rect(canvas, 0.0f, sh * 0.5f, w, sh * 0.2f, c3(120, 146, 152),
                  c3(96, 122, 130));
    for (int32_t i = 0; i < 8; ++i) {
      const float y = sh * (0.52f + 0.022f * (float)i);
      capsule(canvas, w * hash_unit(61u, (uint32_t)i), y,
              w * hash_unit(61u, (uint32_t)i) + 60.0f * u, y, 1.0f * u,
              RGB(190, 210, 214), 140u);
    }
    gradient_rect(canvas, 0.0f, sh * 0.7f, w, sh * 0.3f, c3(132, 140, 98),
                  c3(96, 104, 70));
    for (int32_t i = 0; i < 26; ++i) {
      const float x = i < 13 ? w * 0.012f * (float)i
                             : w * (0.85f + 0.012f * (float)(i - 13));
      const float top = sh * (0.32f + 0.2f * hash_unit(67u, (uint32_t)i));
      capsule(canvas, x, sh * 0.98f, x + 6.0f * u, top, 1.4f * u,
              RGB(110, 120, 70), 240u);
      ellipse(canvas, x + 6.0f * u, top, 3.0f * u, 9.0f * u, 0.2f,
              RGB(206, 190, 146), 230u);
    }
    for (int32_t i = 0; i < 5; ++i)
      fill_rect(canvas, 0.0f, sh * (0.18f + 0.12f * (float)i), w, 10.0f * u,
                RGB(236, 240, 242), 70u);
  }
  fill_rect(canvas, 0.0f, (float)l->panel.y, w, (float)l->panel.h,
            RGB(20, 16, 24), 256u);
}

static void battle_backdrop(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float w = (float)canvas->width, sh = (float)l->stage.h;
  const float u = game->unit;
  const bool shrine = game->map == H2_XIANXIA_RPG_MAP_SHRINE ||
                      game->map == H2_XIANXIA_RPG_MAP_LANTERN_ALTAR;
  if (h2_xianxia_rpg_map_chapter(game->map) == 3u ||
      game->map == H2_XIANXIA_RPG_MAP_WEN_TOWER) {
    gradient_rect(canvas, 0, 0, w, sh, c3(18, 36, 56), c3(80, 104, 120));
    mountains(canvas, sh * 0.18f, sh * 0.76f, c3(40, 64, 82), c3(24, 46, 64),
              c3(76, 100, 116), 1.8f);
    for (int i = 0; i < 7; ++i) {
      float px = w * (float)i / 6.0f;
      fill_rect(canvas, px, 0, w * 0.025f, sh * 0.8f, RGB(70, 42, 42), 256u);
    }
    fill_rect(canvas, 0, sh * 0.14f, w, sh * 0.06f, RGB(34, 32, 42), 256u);
    gradient_rect(canvas, 0, sh * 0.76f, w, sh * 0.24f, c3(84, 108, 116),
                  c3(40, 60, 74));
    for (int i = 0; i < 5; ++i)
      fill_rect(canvas, 0, sh * (0.78f + 0.045f * (float)i), w, 2,
                RGB(30, 48, 62), 200u);
    fill_rect(canvas, 0, (float)l->panel.y, w, (float)l->panel.h,
              RGB(20, 16, 24), 256u);
    return;
  }
  if (h2_xianxia_rpg_map_chapter(game->map) == 2u || game->chapter == 5u) {
    river_backdrop(canvas, game);
    return;
  }
  if (shrine) {
    gradient_rect(canvas, 0.0f, 0.0f, w, sh * 0.62f, c3(30, 24, 48),
                  c3(70, 52, 92));
    mountains(canvas, sh * 0.15f, sh * 0.62f, c3(54, 44, 74), c3(30, 26, 42),
              c3(70, 52, 92), 2.4f);
    gradient_rect(canvas, 0.0f, sh * 0.62f, w, sh * 0.38f, c3(70, 64, 82),
                  c3(40, 36, 48));
    for (int32_t i = 0; i < 7; ++i) {
      const float y = sh * (0.62f + 0.055f * (float)(i * i) / 3.0f);
      if (y < sh) fill_rect(canvas, 0.0f, y, w, 1.0f, RGB(30, 26, 36), 200u);
    }
    for (int32_t i = 0; i < 2; ++i) {
      const float x = i == 0 ? w * 0.06f : w * 0.94f;
      fill_rect(canvas, x - 12.0f * u, sh * 0.08f, 24.0f * u, sh * 0.62f,
                RGB(120, 34, 32), 256u);
      fill_rect(canvas, x - 12.0f * u, sh * 0.08f, 6.0f * u, sh * 0.62f,
                RGB(170, 64, 54), 200u);
      fill_rect(canvas, x - 16.0f * u, sh * 0.06f, 32.0f * u, 8.0f * u,
                RGB(190, 150, 70), 256u);
    }
    for (int32_t i = 0; i < 9; ++i)
      glow(canvas, w * hash_unit(17u, (uint32_t)i),
           sh * (0.55f + 0.35f * hash_unit(23u, (uint32_t)i)),
           (30.0f + 20.0f * hash_unit(31u, (uint32_t)i)) * u, RGB(120, 90, 170),
           70u);
  } else {
    gradient_rect(canvas, 0.0f, 0.0f, w, sh * 0.6f, c3(206, 222, 210),
                  c3(232, 236, 220));
    mountains(canvas, sh * 0.12f, sh * 0.6f, c3(150, 176, 168), c3(80, 112, 96),
              c3(228, 234, 220), 0.7f);
    gradient_rect(canvas, 0.0f, sh * 0.6f, w, sh * 0.4f, c3(112, 150, 88),
                  c3(70, 104, 60));
    for (int32_t i = 0; i < 30; ++i) {
      const float x = w * hash_unit(5u, (uint32_t)i);
      const float y = sh * (0.64f + 0.34f * hash_unit(9u, (uint32_t)i));
      capsule(canvas, x, y, x + 3.0f * u, y - 7.0f * u, 1.0f * u,
              RGB(54, 92, 50), 200u);
    }
    for (int32_t i = 0; i < 6; ++i) {
      const float x = i < 3 ? w * (0.02f + 0.05f * (float)i)
                            : w * (0.88f + 0.05f * (float)(i - 3));
      bamboo_stalk(canvas, x, 0.0f, sh * 0.8f, (7.0f - (float)(i % 3)) * u,
                   i % 2 ? RGB(60, 104, 64) : RGB(38, 74, 48), 256u);
      bamboo_leaves(canvas, x, sh * (0.15f + 0.12f * (float)(i % 3)), 22.0f * u,
                    RGB(46, 92, 54), 230u, (uint32_t)i + 40u);
    }
  }
  /* Panel base so its bands never show stale pixels. */
  fill_rect(canvas, 0.0f, (float)l->panel.y, w, (float)l->panel.h,
            RGB(20, 16, 24), 256u);
}

/* ---- Map tiles --------------------------------------------------------- */

typedef struct ground {
  color3_t grass, grass_dark, path, water_top, water_bottom, water_line;
} ground_t;

static ground_t ground_for(h2_xianxia_rpg_map_t map, bool cleared) {
  switch (map) {
    case H2_XIANXIA_RPG_MAP_FOREST:
      return (ground_t){c3(74, 128, 66), c3(48, 92, 48), c3(170, 150, 104),
                        c3(40, 80, 96),  c3(30, 60, 76), c3(110, 160, 170)};
    case H2_XIANXIA_RPG_MAP_SHRINE:
      return (ground_t){c3(58, 92, 60), c3(38, 64, 44), c3(150, 140, 120),
                        c3(54, 34, 70), c3(26, 18, 38), c3(150, 110, 190)};
    case H2_XIANXIA_RPG_MAP_DOCK:
      if (!cleared)
        return (ground_t){c3(118, 136, 98), c3(88, 106, 76), c3(170, 154, 120),
                          c3(70, 92, 100),  c3(48, 68, 78),  c3(150, 172, 176)};
      return (ground_t){c3(118, 150, 92), c3(88, 118, 70), c3(186, 166, 124),
                        c3(56, 112, 142), c3(38, 86, 118), c3(172, 212, 226)};
    case H2_XIANXIA_RPG_MAP_REEDS:
      return (ground_t){c3(112, 132, 82), c3(82, 102, 62), c3(160, 146, 108),
                        c3(62, 90, 96),   c3(44, 68, 76),  c3(140, 170, 170)};
    case H2_XIANXIA_RPG_MAP_WRECK:
      if (!cleared)
        return (ground_t){c3(190, 178, 142), c3(164, 150, 116),
                          c3(190, 178, 142), c3(58, 82, 92),
                          c3(38, 58, 68),    c3(128, 156, 166)};
      return (ground_t){c3(206, 192, 150), c3(178, 162, 122),
                        c3(206, 192, 150), c3(56, 108, 138),
                        c3(38, 82, 112),   c3(170, 210, 224)};
    default:
      if (!cleared)
        return (ground_t){c3(104, 158, 76), c3(70, 120, 58), c3(198, 172, 120),
                          c3(36, 40, 52),   c3(18, 20, 28),  c3(96, 92, 124)};
      return (ground_t){c3(104, 158, 76), c3(70, 120, 58), c3(198, 172, 120),
                        c3(44, 74, 92),   c3(32, 54, 70),  c3(120, 160, 176)};
  }
}

static void noise_fill(canvas_t* canvas, float x, float y, float t,
                       color3_t base, color3_t dark, float speckle,
                       uint32_t seed) {
  region_t g;
  if (!region_begin(canvas, x, y, x + t, y + t, &g)) return;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    uint16_t* row = row_at(canvas, py);
    for (int32_t px = g.x0; px < g.x1; ++px) {
      const uint32_t h = hash32((uint32_t)px * 2654435761u ^
                                (uint32_t)py * 2246822519u ^ seed);
      const float n = (float)(h & 255u) / 255.0f;
      color3_t c = scaled(base, 0.94f + 0.12f * n);
      if ((h >> 8 & 63u) < (uint32_t)(speckle * 64.0f)) c = dark;
      row[px] = pack(c);
    }
  }
}

static void grass(canvas_t* canvas, const ground_t* gr, float x, float y,
                  float t, uint32_t seed) {
  noise_fill(canvas, x, y, t, gr->grass, gr->grass_dark, 0.05f, seed);
  for (uint32_t i = 0; i < 3u; ++i) {
    const float bx = x + t * (0.15f + 0.7f * hash_unit(seed, i));
    const float by = y + t * (0.2f + 0.7f * hash_unit(seed, i + 9u));
    capsule(canvas, bx, by, bx - t * 0.04f, by - t * 0.12f, t * 0.018f,
            pack(gr->grass_dark), 220u);
    capsule(canvas, bx, by, bx + t * 0.05f, by - t * 0.1f, t * 0.018f,
            pack(gr->grass_dark), 220u);
  }
}

static void stone_floor(canvas_t* canvas, float x, float y, float t,
                        uint32_t seed) {
  noise_fill(canvas, x, y, t, c3(128, 122, 132), c3(100, 96, 106), 0.04f, seed);
  fill_rect(canvas, x, y + t - 1.0f, t, 1.0f, RGB(84, 80, 92), 256u);
  fill_rect(canvas, x + t - 1.0f, y, 1.0f, t, RGB(84, 80, 92), 256u);
  fill_rect(canvas, x, y, t, 1.0f, RGB(150, 146, 154), 160u);
}

static void water(canvas_t* canvas, const ground_t* gr, float x, float y,
                  float t, uint32_t seed) {
  gradient_rect(canvas, x, y, t, t, gr->water_top, gr->water_bottom);
  for (uint32_t i = 0; i < 2u; ++i) {
    const float wx = x + t * (0.1f + 0.5f * hash_unit(seed, i));
    const float wy = y + t * (0.25f + 0.5f * hash_unit(seed, i + 4u));
    capsule(canvas, wx, wy, wx + t * 0.3f, wy, t * 0.025f, pack(gr->water_line),
            150u);
  }
}

/* The boat Bing San, bow to the right; the bow lamp glows when lit. */
static void boat_art(canvas_t* canvas, float x, float y, float t, bool lit) {
  ellipse(canvas, x + t * 0.5f, y + t * 0.78f, t * 0.5f, t * 0.1f, 0.0f,
          RGB(20, 30, 36), 110u);
  quad(canvas, x - t * 0.02f, y + t * 0.42f, x + t * 1.02f, y + t * 0.36f,
       x + t * 0.84f, y + t * 0.74f, x + t * 0.12f, y + t * 0.74f,
       RGB(112, 74, 44), 256u);
  fill_rect(canvas, x + t * 0.04f, y + t * 0.4f, t * 0.92f, t * 0.06f,
            RGB(176, 128, 78), 256u);
  fill_rect(canvas, x + t * 0.2f, y + t * 0.24f, t * 0.36f, t * 0.18f,
            RGB(84, 62, 44), 256u);
  fill_rect(canvas, x + t * 0.16f, y + t * 0.2f, t * 0.44f, t * 0.06f,
            RGB(60, 46, 34), 256u);
  capsule(canvas, x + t * 0.86f, y + t * 0.38f, x + t * 0.86f, y + t * 0.06f,
          t * 0.025f, RGB(70, 50, 34), 256u);
  if (lit)
    glow(canvas, x + t * 0.86f, y + t * 0.08f, t * 0.7f, RGB(200, 255, 240),
         170u);
  rounded_rect(canvas, x + t * 0.8f, y + t * 0.02f, x + t * 0.92f,
               y + t * 0.16f, t * 0.03f,
               lit ? RGB(232, 252, 236) : RGB(90, 96, 100), 256u);
}

/* A wooden lamp post; lit ones throw the companion's pale green-white
 * light. */
static void lamp_post(canvas_t* canvas, float x, float y, float t, bool lit) {
  ellipse(canvas, x + t * 0.5f, y + t * 0.9f, t * 0.26f, t * 0.07f, 0.0f,
          C_SHADOW, 120u);
  fill_rect(canvas, x + t * 0.44f, y + t * 0.18f, t * 0.12f, t * 0.74f,
            RGB(92, 66, 42), 256u);
  fill_rect(canvas, x + t * 0.3f, y + t * 0.14f, t * 0.4f, t * 0.06f,
            RGB(70, 50, 32), 256u);
  if (lit)
    glow(canvas, x + t * 0.5f, y + t * 0.1f, t * 0.95f, RGB(200, 255, 236),
         190u);
  rounded_rect(canvas, x + t * 0.36f, y - t * 0.12f, x + t * 0.64f,
               y + t * 0.16f, t * 0.05f,
               lit ? RGB(236, 255, 240) : RGB(74, 80, 84), 256u);
  fill_rect(canvas, x + t * 0.36f, y - t * 0.14f, t * 0.28f, t * 0.04f,
            RGB(60, 44, 30), 256u);
  if (lit)
    disc(canvas, x + t * 0.5f, y + t * 0.03f, t * 0.06f, RGB(255, 250, 200),
         256u);
}

static void reed_stalks(canvas_t* canvas, float x, float y, float t,
                        uint32_t seed, uint32_t count, uint16_t stalk,
                        uint16_t plume) {
  for (uint32_t i = 0; i < count; ++i) {
    const float sx = x + t * (0.1f + 0.8f * hash_unit(seed, i + 40u));
    const float top = y + t * (0.02f + 0.3f * hash_unit(seed, i + 50u));
    const float lean = t * 0.12f * (hash_unit(seed, i + 60u) - 0.5f);
    capsule(canvas, sx, y + t * 0.96f, sx + lean, top, t * 0.022f, stalk, 240u);
    ellipse(canvas, sx + lean, top, t * 0.05f, t * 0.12f, lean / t, plume,
            230u);
  }
}

static void planks(canvas_t* canvas, float x, float y, float t, bool old_wood,
                   uint32_t seed) {
  noise_fill(canvas, x, y, t, old_wood ? c3(112, 94, 76) : c3(150, 112, 72),
             old_wood ? c3(86, 72, 58) : c3(122, 88, 56), 0.04f, seed);
  for (int32_t i = 1; i < 4; ++i)
    fill_rect(canvas, x, y + t * 0.25f * (float)i, t, fmaxf(1.0f, t * 0.03f),
              old_wood ? RGB(66, 54, 44) : RGB(96, 66, 40), 256u);
  disc(canvas, x + t * 0.12f, y + t * 0.12f, t * 0.025f, RGB(60, 46, 36), 200u);
  disc(canvas, x + t * 0.88f, y + t * 0.62f, t * 0.025f, RGB(60, 46, 36), 200u);
}

/* Ground of a chapter-2 map: packed earth at the dock, wet grass in the
 * reeds, sand on the bar. */
static void c2_ground(canvas_t* canvas, const game_t* game, const ground_t* gr,
                      float x, float y, float t, uint32_t seed) {
  if (game->map == H2_XIANXIA_RPG_MAP_REEDS)
    grass(canvas, gr, x, y, t, seed);
  else
    noise_fill(canvas, x, y, t, gr->path, scaled(gr->path, 0.84f), 0.06f, seed);
}

static bool c2_tile_art(canvas_t* canvas, const game_t* game, char tile,
                        const ground_t* gr, float x, float y, float t,
                        uint32_t seed, int32_t col, int32_t row) {
  const h2_xianxia_rpg_map_t map = game->map;
  const uint32_t flags = game->flags;
  const bool clear = game->stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN;
  switch (tile) {
    case '.':
      c2_ground(canvas, game, gr, x, y, t, seed);
      return true;
    case 'p': {
      const bool water_sides = h2_xianxia_rpg_tile(map, col - 1, row) == '~' &&
                               h2_xianxia_rpg_tile(map, col + 1, row) == '~';
      if (!water_sides) {
        planks(canvas, x, y, t, map == H2_XIANXIA_RPG_MAP_WRECK, seed);
        return true;
      }
      water(canvas, gr, x, y, t, seed);
      fill_rect(canvas, x + t * 0.14f, y, t * 0.72f, t, RGB(150, 112, 72),
                256u);
      for (int32_t i = 1; i < 4; ++i)
        fill_rect(canvas, x + t * 0.14f, y + t * 0.25f * (float)i, t * 0.72f,
                  fmaxf(1.0f, t * 0.03f), RGB(96, 66, 40), 256u);
      fill_rect(canvas, x + t * 0.1f, y, t * 0.07f, t, RGB(80, 56, 34), 256u);
      fill_rect(canvas, x + t * 0.83f, y, t * 0.07f, t, RGB(80, 56, 34), 256u);
      return true;
    }
    case 'n':
      c2_ground(canvas, game, gr, x, y, t, seed);
      fill_rect(canvas, x + t * 0.16f, y + t * 0.3f, t * 0.08f, t * 0.62f,
                RGB(92, 64, 40), 256u);
      fill_rect(canvas, x + t * 0.76f, y + t * 0.3f, t * 0.08f, t * 0.62f,
                RGB(92, 64, 40), 256u);
      rounded_rect(canvas, x + t * 0.08f, y + t * 0.08f, x + t * 0.92f,
                   y + t * 0.6f, t * 0.04f, RGB(150, 112, 70), 256u);
      fill_rect(canvas, x + t * 0.18f, y + t * 0.16f, t * 0.34f, t * 0.36f,
                RGB(242, 234, 214), 256u);
      fill_rect(canvas, x + t * 0.56f, y + t * 0.2f, t * 0.28f, t * 0.28f,
                RGB(232, 224, 200), 256u);
      for (int32_t i = 0; i < 3; ++i)
        fill_rect(canvas, x + t * 0.22f, y + t * (0.22f + 0.09f * (float)i),
                  t * 0.24f, fmaxf(1.0f, t * 0.025f), RGB(70, 60, 56), 220u);
      disc(canvas, x + t * 0.7f, y + t * 0.36f, t * 0.06f, RGB(196, 50, 40),
           230u);
      return true;
    case 't':
      c2_ground(canvas, game, gr, x, y, t, seed);
      if (h2_xianxia_rpg_tile(map, col, row - 1) == 't') {
        /* Tea table with a pot and cups. */
        ellipse(canvas, x + t * 0.5f, y + t * 0.86f, t * 0.4f, t * 0.08f, 0.0f,
                C_SHADOW, 110u);
        fill_rect(canvas, x + t * 0.08f, y + t * 0.4f, t * 0.84f, t * 0.14f,
                  RGB(128, 88, 54), 256u);
        fill_rect(canvas, x + t * 0.14f, y + t * 0.54f, t * 0.08f, t * 0.34f,
                  RGB(96, 64, 40), 256u);
        fill_rect(canvas, x + t * 0.78f, y + t * 0.54f, t * 0.08f, t * 0.34f,
                  RGB(96, 64, 40), 256u);
        disc(canvas, x + t * 0.4f, y + t * 0.32f, t * 0.12f, RGB(110, 140, 120),
             256u);
        capsule(canvas, x + t * 0.5f, y + t * 0.3f, x + t * 0.6f, y + t * 0.24f,
                t * 0.025f, RGB(110, 140, 120), 256u);
        disc(canvas, x + t * 0.72f, y + t * 0.36f, t * 0.05f,
             RGB(240, 236, 224), 256u);
      } else {
        /* Striped awning on two poles. */
        fill_rect(canvas, x + t * 0.08f, y + t * 0.3f, t * 0.06f, t * 0.7f,
                  RGB(92, 64, 40), 256u);
        fill_rect(canvas, x + t * 0.86f, y + t * 0.3f, t * 0.06f, t * 0.7f,
                  RGB(92, 64, 40), 256u);
        quad(canvas, x - t * 0.04f, y + t * 0.36f, x + t * 1.04f, y + t * 0.36f,
             x + t * 0.92f, y + t * 0.06f, x + t * 0.08f, y + t * 0.06f,
             RGB(196, 64, 48), 256u);
        for (int32_t i = 0; i < 3; ++i)
          fill_rect(canvas, x + t * (0.14f + 0.28f * (float)i), y + t * 0.08f,
                    t * 0.12f, t * 0.27f, RGB(244, 230, 200), 230u);
      }
      return true;
    case 'm':
      c2_ground(canvas, game, gr, x, y, t, seed);
      fill_rect(canvas, x + t * 0.08f, y + t * 0.1f, t * 0.06f, t * 0.84f,
                RGB(92, 64, 40), 256u);
      fill_rect(canvas, x + t * 0.86f, y + t * 0.1f, t * 0.06f, t * 0.84f,
                RGB(92, 64, 40), 256u);
      fill_rect(canvas, x + t * 0.08f, y + t * 0.14f, t * 0.84f, t * 0.04f,
                RGB(92, 64, 40), 256u);
      for (int32_t i = 0; i < 5; ++i) {
        const float k = 0.16f + 0.17f * (float)i;
        capsule(canvas, x + t * k, y + t * 0.18f, x + t * (k + 0.1f),
                y + t * 0.8f, t * 0.012f, RGB(70, 76, 70), 220u);
        capsule(canvas, x + t * 0.14f, y + t * (0.18f + 0.15f * (float)i),
                x + t * 0.86f, y + t * (0.22f + 0.15f * (float)i), t * 0.012f,
                RGB(70, 76, 70), 220u);
      }
      return true;
    case 'j':
      water(canvas, gr, x, y, t, seed);
      fill_rect(canvas, x + t * 0.08f, y + t * 0.05f, t * 0.12f, t * 0.6f,
                RGB(70, 50, 34), 256u);
      if (clear) boat_art(canvas, x, y + t * 0.1f, t, true);
      return true;
    case 'i':
      noise_fill(canvas, x, y, t, c3(62, 78, 52), c3(44, 58, 40), 0.08f, seed);
      reed_stalks(canvas, x, y - t * 0.1f, t, seed, 7u, RGB(96, 112, 64),
                  RGB(150, 136, 100));
      return true;
    case 'v':
      c2_ground(canvas, game, gr, x, y, t, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.88f, t * 0.42f, t * 0.1f, 0.0f,
              RGB(50, 64, 40), 120u);
      reed_stalks(canvas, x, y, t, seed, 5u, RGB(128, 140, 80),
                  RGB(214, 196, 150));
      return true;
    case 'F':
    case 'G': {
      const bool lifted =
          (flags & (tile == 'F' ? H2_XIANXIA_RPG_FLAG_C2_POST1
                                : H2_XIANXIA_RPG_FLAG_C2_POST2)) != 0u;
      c2_ground(canvas, game, gr, x, y, t, seed);
      if (lifted) return true;
      fill_rect(canvas, x, y, t, t, RGB(222, 228, 232), 170u);
      for (uint32_t i = 0; i < 3u; ++i)
        glow(canvas, x + t * (0.2f + 0.6f * hash_unit(seed, i + 70u)),
             y + t * (0.2f + 0.6f * hash_unit(seed, i + 80u)), t * 0.6f,
             RGB(246, 248, 250), 200u);
      capsule(canvas, x + t * 0.1f, y + t * 0.3f, x + t * 0.8f, y + t * 0.36f,
              t * 0.03f, RGB(190, 200, 210), 160u);
      capsule(canvas, x + t * 0.3f, y + t * 0.7f, x + t * 0.95f, y + t * 0.64f,
              t * 0.03f, RGB(190, 200, 210), 160u);
      return true;
    }
    case '1':
    case '2':
    case '4': {
      const uint32_t lit_flag = tile == '1'   ? H2_XIANXIA_RPG_FLAG_C2_POST1
                                : tile == '2' ? H2_XIANXIA_RPG_FLAG_C2_POST2
                                              : H2_XIANXIA_RPG_FLAG_C2_POST4;
      c2_ground(canvas, game, gr, x, y, t, seed);
      lamp_post(canvas, x, y, t, (flags & lit_flag) != 0u);
      return true;
    }
    case 'U':
      water(canvas, gr, x, y, t, seed);
      boat_art(canvas, x, y, t,
               map == H2_XIANXIA_RPG_MAP_WRECK ||
                   (flags & H2_XIANXIA_RPG_FLAG_C2_BOAT) != 0u);
      return true;
    case 'c':
      c2_ground(canvas, game, gr, x, y, t, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.8f, t * 0.24f, t * 0.08f, 0.0f,
              C_SHADOW, 110u);
      fill_rect(canvas, x + t * 0.38f, y + t * 0.3f, t * 0.24f, t * 0.5f,
                RGB(104, 74, 48), 256u);
      ellipse(canvas, x + t * 0.5f, y + t * 0.3f, t * 0.12f, t * 0.05f, 0.0f,
              RGB(150, 116, 80), 256u);
      ellipse(canvas, x + t * 0.5f, y + t * 0.56f, t * 0.16f, t * 0.06f, 0.0f,
              RGB(200, 176, 120), 256u);
      capsule(canvas, x + t * 0.62f, y + t * 0.56f, x + t * 0.86f, y + t * 0.5f,
              t * 0.03f, RGB(200, 176, 120), 256u);
      /* The clean cut at the rope's end. */
      fill_rect(canvas, x + t * 0.86f, y + t * 0.44f, t * 0.02f, t * 0.12f,
                RGB(250, 244, 220), 256u);
      return true;
    case 'k': {
      const bool empty = (flags & H2_XIANXIA_RPG_FLAG_C2_BASKET) != 0u;
      c2_ground(canvas, game, gr, x, y, t, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.82f, t * 0.3f, t * 0.08f, 0.0f,
              C_SHADOW, 110u);
      ellipse(canvas, x + t * 0.5f, y + t * 0.6f, t * 0.28f, t * 0.24f,
              empty ? 0.5f : 0.0f, RGB(170, 132, 70), 256u);
      for (int32_t i = 0; i < 3; ++i)
        fill_rect(canvas, x + t * 0.26f, y + t * (0.48f + 0.1f * (float)i),
                  t * 0.48f, fmaxf(1.0f, t * 0.025f), RGB(120, 88, 44), 220u);
      ellipse(canvas, x + t * 0.5f, y + t * 0.4f, t * 0.2f, t * 0.06f, 0.0f,
              RGB(90, 66, 34), 256u);
      if (!empty)
        rounded_rect(canvas, x + t * 0.4f, y + t * 0.3f, x + t * 0.6f,
                     y + t * 0.4f, t * 0.03f, RGB(220, 196, 130), 256u);
      return true;
    }
    case '|':
      noise_fill(canvas, x, y, t, c3(78, 66, 58), c3(58, 48, 42), 0.06f, seed);
      for (int32_t i = 1; i < 4; ++i)
        fill_rect(canvas, x + t * 0.25f * (float)i, y, fmaxf(1.0f, t * 0.03f),
                  t, RGB(44, 36, 32), 256u);
      fill_rect(canvas, x, y + t * 0.86f, t, t * 0.14f, RGB(36, 30, 28), 220u);
      disc(canvas, x + t * 0.5f, y + t * 0.2f, t * 0.03f, RGB(120, 110, 96),
           200u);
      return true;
    case 'x':
      planks(canvas, x, y, t, true, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.86f, t * 0.4f, t * 0.08f, 0.0f,
              C_SHADOW, 120u);
      fill_rect(canvas, x + t * 0.12f, y + t * 0.18f, t * 0.76f, t * 0.66f,
                RGB(132, 98, 62), 256u);
      fill_rect(canvas, x + t * 0.12f, y + t * 0.18f, t * 0.76f, t * 0.08f,
                RGB(164, 126, 82), 256u);
      capsule(canvas, x + t * 0.16f, y + t * 0.3f, x + t * 0.84f, y + t * 0.8f,
              t * 0.03f, RGB(92, 66, 40), 256u);
      capsule(canvas, x + t * 0.84f, y + t * 0.3f, x + t * 0.16f, y + t * 0.8f,
              t * 0.03f, RGB(92, 66, 40), 256u);
      return true;
    case 'w':
      planks(canvas, x, y, t, true, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.55f, t * 0.42f, t * 0.28f, 0.0f,
              RGB(60, 86, 96), 200u);
      capsule(canvas, x + t * 0.3f, y + t * 0.5f, x + t * 0.6f, y + t * 0.5f,
              t * 0.02f, RGB(150, 180, 186), 160u);
      return true;
    case 'Y':
      planks(canvas, x, y, t, true, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.7f, t * 0.46f, t * 0.2f, 0.0f,
              RGB(60, 86, 96), 220u);
      glow(canvas, x + t * 0.5f, y + t * 0.4f, t * 1.1f,
           clear ? RGB(255, 214, 120) : RGB(150, 100, 210), 170u);
      fill_rect(canvas, x + t * 0.32f, y + t * 0.18f, t * 0.36f, t * 0.06f,
                RGB(70, 54, 40), 256u);
      rounded_rect(canvas, x + t * 0.3f, y + t * 0.24f, x + t * 0.7f,
                   y + t * 0.66f, t * 0.06f,
                   clear ? RGB(255, 236, 170) : RGB(110, 96, 130), 256u);
      fill_rect(canvas, x + t * 0.3f, y + t * 0.64f, t * 0.4f, t * 0.06f,
                RGB(70, 54, 40), 256u);
      if (!clear)
        capsule(canvas, x + t * 0.36f, y + t * 0.3f, x + t * 0.56f,
                y + t * 0.56f, t * 0.015f, RGB(40, 30, 50), 256u);
      return true;
    default:
      return false;
  }
}

/* A wet slate court. Pale sword tips are the puzzle's readable direction;
 * the gold center is their common destination, not another movable post. */
static void c3_tile_art(canvas_t* canvas, const game_t* game, char tile,
                        float x, float y, float t, uint32_t seed) {
  const bool quiet = game->stage >= H2_XIANXIA_RPG_STAGE_C3_RETURN;
  noise_fill(canvas, x, y, t, quiet ? c3(90, 116, 124) : c3(60, 80, 96),
             c3(46, 62, 76), 0.04f, seed);
  fill_rect(canvas, x, y + t - 1, t, 1, RGB(30, 44, 56), 230u);
  fill_rect(canvas, x + t - 1, y, 1, t, RGB(30, 44, 56), 230u);
  const float cx = x + t * 0.5f, cy = y + t * 0.5f;
  if (tile == 'T' || tile == '~') {
    fill_rect(canvas, x, y, t, t, RGB(24, 42, 58), 256u);
    if (tile == 'T') {
      fill_rect(canvas, cx - t * 0.05f, y + t * 0.3f, t * 0.1f, t * 0.7f,
                RGB(66, 68, 66), 256u);
      triangle(canvas, x, y + t * 0.7f, x + t, y + t * 0.7f, cx, y - t * 0.05f,
               RGB(32, 66, 66), 256u);
      triangle(canvas, x + t * 0.12f, cy, x + t * 0.88f, cy, cx, y - t * 0.15f,
               RGB(44, 82, 78), 256u);
    } else
      capsule(canvas, x + t * 0.2f, cy, x + t * 0.7f, cy, t * 0.02f,
              RGB(90, 134, 156), 160u);
  } else if (tile == '#' || tile == 'R') {
    fill_rect(canvas, x, y, t, t,
              tile == 'R' ? RGB(26, 38, 52) : RGB(38, 52, 64), 256u);
    for (int i = 0; i < 4; ++i)
      fill_rect(canvas, x, y + t * 0.25f * (float)i, t, t * 0.04f,
                RGB(82, 104, 118), 210u);
  } else if (tile == 's') {
    for (int i = 0; i < 4; ++i)
      fill_rect(canvas, x + t * 0.08f, y + t * 0.25f * (float)i, t * 0.84f,
                t * 0.08f, RGB(148, 162, 166), 220u);
  } else if (tile == 'v') {
    reed_stalks(canvas, x, y, t, seed, 4u, RGB(52, 100, 108),
                RGB(150, 178, 182));
  } else if (tile == 'P') {
    fill_rect(canvas, x + t * 0.3f, y, t * 0.4f, t * 0.88f, RGB(90, 52, 48),
              256u);
    fill_rect(canvas, x + t * 0.24f, y + t * 0.85f, t * 0.52f, t * 0.12f,
              RGB(156, 136, 102), 256u);
  } else if (tile == 'L') {
    lamp_post(canvas, x, y, t, true);
  } else if (tile == 'Q') {
    fill_rect(canvas, x + t * 0.1f, y + t * 0.4f, t * 0.08f, t * 0.55f,
              RGB(160, 118, 78), 256u);
    fill_rect(canvas, x + t * 0.82f, y + t * 0.4f, t * 0.08f, t * 0.55f,
              RGB(160, 118, 78), 256u);
    triangle(canvas, x - t * 0.08f, cy, x + t * 1.08f, cy, cx, y,
             RGB(118, 76, 64), 256u);
    disc(canvas, cx, y + t * 0.76f, t * 0.12f, RGB(250, 202, 122), 256u);
  } else if (tile == 'n' || tile == 'd') {
    fill_rect(canvas, x + t * 0.22f, y + t * 0.18f, t * 0.56f, t * 0.66f,
              RGB(224, 208, 170), 256u);
    for (int i = 0; i < 3; ++i)
      capsule(canvas, x + t * 0.35f, y + t * (0.32f + 0.16f * (float)i),
              x + t * 0.65f, y + t * (0.32f + 0.16f * (float)i), t * 0.025f,
              RGB(86, 64, 60), 240u);
    if (tile == 'd')
      capsule(canvas, x + t * 0.1f, y + t * 0.65f, x + t * 0.9f, y + t * 0.42f,
              t * 0.055f, RGB(30, 44, 56), 256u);
  } else if (tile >= '1' && tile <= '3') {
    const uint32_t dir =
        h2_xianxia_rpg_c3_post_direction(game->flags, (uint32_t)(tile - '1'));
    static const float dx[] = {0, 1, 0, -1}, dy[] = {-1, 0, 1, 0};
    const float vx = dx[dir], vy = dy[dir];
    disc(canvas, cx, cy, t * 0.38f, RGB(26, 38, 48), 256u);
    disc(canvas, cx, cy, t * 0.3f, RGB(120, 138, 144), 256u);
    capsule(canvas, cx - vx * t * 0.22f, cy - vy * t * 0.22f,
            cx + vx * t * 0.16f, cy + vy * t * 0.16f, t * 0.06f,
            RGB(230, 244, 242), 256u);
    triangle(canvas, cx + vx * t * 0.45f, cy + vy * t * 0.45f,
             cx + vx * t * 0.1f - vy * t * 0.18f,
             cy + vy * t * 0.1f + vx * t * 0.18f,
             cx + vx * t * 0.1f + vy * t * 0.18f,
             cy + vy * t * 0.1f - vx * t * 0.18f, C_GOLD, 256u);
    capsule(canvas, cx - vx * t * 0.16f - vy * t * 0.13f,
            cy - vy * t * 0.16f + vx * t * 0.13f,
            cx - vx * t * 0.16f + vy * t * 0.13f,
            cy - vy * t * 0.16f - vx * t * 0.13f, t * 0.035f, RGB(54, 64, 72),
            256u);
  } else if (tile == 'X') {
    if (!(game->flags & H2_XIANXIA_RPG_FLAG_C3_OPEN))
      for (int i = 0; i < 3; ++i) {
        const float sx = x + t * (0.2f + (float)i * 0.3f);
        capsule(canvas, sx, y + t * 0.1f, sx, y + t * 0.9f, t * 0.055f,
                RGB(190, 232, 250), 230u);
        capsule(canvas, sx - t * 0.1f, y + t * 0.25f, sx + t * 0.1f,
                y + t * 0.25f, t * 0.03f, C_GOLD, 256u);
      }
  } else if (tile == 'O' || tile == 'a' || tile == 'b' || tile == 'c') {
    fill_rect(canvas, x + t * 0.15f, y + t * 0.2f, t * 0.7f, t * 0.65f,
              RGB(124, 140, 146), 256u);
    fill_rect(canvas, x + t * 0.1f, y + t * 0.8f, t * 0.8f, t * 0.12f,
              RGB(46, 58, 66), 256u);
    if (tile == 'O') {
      diamond(canvas, cx, cy, t * 0.22f, C_GOLD, 256u);
    } else if (tile == 'c') {
      ellipse(canvas, cx, cy, t * 0.23f, t * 0.17f, -0.3f, RGB(226, 242, 236),
              256u);
      ellipse(canvas, cx, cy, t * 0.16f, t * 0.1f, -0.3f, RGB(124, 140, 146),
              256u);
    } else {
      for (int i = 0; i < (tile == 'a' ? 3 : 1); ++i) {
        const float sx = cx + (tile == 'a' ? (float)(i - 1) * t * 0.16f : 0);
        capsule(canvas, sx, y + t * 0.3f, sx, y + t * 0.7f, t * 0.025f,
                RGB(226, 242, 236), 256u);
      }
    }
  }
}

static void c4_tile_art(canvas_t* canvas, const game_t* game, char tile,
                        float x, float y, float t, uint32_t seed) {
  c3_tile_art(
      canvas, game,
      tile == 'w' || tile == '=' || tile == '1' || tile == '2' || tile == '3'
          ? '_'
          : tile,
      x, y, t, seed);
  if (tile == 'w') {
    fill_rect(canvas, x, y, t, t, RGB(46, 76, 108), 160u);
    for (int i = 0; i < 3; ++i) {
      const float yy = y + t * (0.28f + 0.22f * (float)i);
      capsule(canvas, x + t * 0.12f, yy, x + t * 0.88f, yy, t * 0.025f,
              RGB(190, 230, 248), 256u);
      triangle(canvas, x + t * 0.88f, yy, x + t * 0.72f, yy - t * 0.08f,
               x + t * 0.72f, yy + t * 0.08f, RGB(190, 230, 248), 256u);
    }
  } else if (tile >= '1' && tile <= '3') {
    const bool lit = (game->flags & (1u << (tile - '1'))) != 0u;
    lamp_post(canvas, x, y, t, lit);
    char number[2] = {tile, 0};
    text_centered(canvas, false, number, x + t * 0.5f, (int32_t)(y + t * 0.62f),
                  lit ? C_GOLD : C_TEXT, 256u);
  }
}

static void c5_tile_art(canvas_t* canvas, const game_t* game, char tile,
                        float x, float y, float t, uint32_t seed) {
  const bool high = (game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH) != 0u;
  if (tile == 'e')
    tile = game->stage >= H2_XIANXIA_RPG_STAGE_C5_RESCUE &&
                   game->stage <= H2_XIANXIA_RPG_STAGE_C5_ESCORT
               ? 'r'
               : '=';
  const ground_t gr = ground_for(H2_XIANXIA_RPG_MAP_DOCK, true);
  if (tile == '~' || (tile == 'd' && high) || (tile == 'p' && high)) {
    water(canvas, &gr, x, y, t, seed);
    return;
  }
  if (tile == 'r') {
    water(canvas, &gr, x, y, t, seed);
    if (high) {
      fill_rect(canvas, x, y + t * 0.12f, t, t * 0.76f, RGB(78, 145, 149),
                256u);
      for (int i = 1; i < 5; ++i)
        fill_rect(canvas, x, y + t * (float)i * 0.18f, t, 2, RGB(35, 74, 88),
                  256u);
      fill_rect(canvas, x, y, t, t * 0.07f, RGB(210, 219, 172), 256u);
    } else {
      fill_rect(canvas, x, y + t * 0.75f, t, t * 0.15f, RGB(53, 72, 88), 256u);
    }
    return;
  }
  if (tile == 'd' || tile == 'p') {
    noise_fill(canvas, x, y, t, c3(154, 116, 75), c3(119, 89, 62), 0.06f, seed);
    fill_rect(canvas, x, y + t - 2, t, 2, RGB(83, 68, 55), 256u);
    if (tile == 'p' && !(game->flags & H2_XIANXIA_RPG_FLAG_C5_PAGE)) {
      fill_rect(canvas, x + t * 0.25f, y + t * 0.25f, t * 0.5f, t * 0.55f,
                C_GOLD, 256u);
      for (int i = 0; i < 3; ++i)
        fill_rect(canvas, x + t * 0.32f, y + t * (0.35f + (float)i * 0.12f),
                  t * 0.3f, 2, RGB(82, 62, 42), 256u);
    }
    return;
  }
  c3_tile_art(canvas, game,
              tile == '=' || tile == 'B' ? '_'
              : tile == 'h'              ? '#'
                                         : tile,
              x, y, t, seed);
  if (tile == '=') {
    fill_rect(canvas, x, y + t * 0.85f, t, t * 0.1f, RGB(204, 208, 190), 256u);
  } else if (tile == 'h') {
    triangle(canvas, x, y + t * 0.3f, x + t, y + t * 0.3f, x + t * 0.5f, y,
             RGB(147, 83, 61), 256u);
    fill_rect(canvas, x + t * 0.25f, y + t * 0.4f, t * 0.5f, t * 0.25f,
              RGB(222, 171, 94), 256u);
  } else if (tile == 'B') {
    fill_rect(canvas, x + t * 0.12f, y + t * 0.12f, t * 0.76f, t * 0.08f,
              RGB(122, 71, 51), 256u);
    fill_rect(canvas, x + t * 0.13f, y + t * 0.2f, t * 0.07f, t * 0.65f,
              RGB(122, 71, 51), 256u);
    fill_rect(canvas, x + t * 0.8f, y + t * 0.2f, t * 0.07f, t * 0.65f,
              RGB(122, 71, 51), 256u);
    ellipse(canvas, x + t * 0.5f, y + t * 0.47f, t * 0.25f, t * 0.28f, 0,
            C_GOLD, 256u);
    fill_rect(canvas, x + t * 0.23f, y + t * 0.64f, t * 0.54f, t * 0.08f,
              RGB(150, 108, 47), 256u);
    disc(canvas, x + t * 0.5f, y + t * 0.73f, t * 0.06f, C_GOLD, 256u);
  }
}

static void c6_tile_art(canvas_t* canvas, const game_t* game, char tile,
                        float x, float y, float t, uint32_t seed) {
  const bool ice = game->map == H2_XIANXIA_RPG_MAP_FROZEN_CREEK;
  const bool hut = game->map == H2_XIANXIA_RPG_MAP_HERB_HUT;
  noise_fill(canvas, x, y, t, ice ? c3(43, 66, 88) : c3(77, 62, 65),
             ice ? c3(33, 51, 72) : c3(56, 46, 52), 0.04f, seed);
  if (tile == '#') {
    fill_rect(canvas, x + t * .08f, y + t * .18f, t * .84f, t * .75f,
              ice ? RGB(62, 89, 112) : RGB(94, 79, 84), 256u);
    fill_rect(canvas, x + t * .08f, y + t * .12f, t * .84f, t * .18f,
              RGB(203, 217, 219), 256u);
    return;
  }
  if (hut && tile == 'h') {
    fill_rect(canvas, x, y + t * .3f, t, t * .7f, RGB(82, 59, 54), 256u);
    triangle(canvas, x, y + t * .35f, x + t, y + t * .35f, x + t * .5f, y,
             RGB(119, 69, 58), 256u);
    fill_rect(canvas, x + t * .27f, y + t * .48f, t * .46f, t * .28f, C_GOLD,
              256u);
    return;
  }
  if (tile == '~') {
    fill_rect(canvas, x, y, t, t, RGB(70, 98, 104), 256u);
    fill_rect(canvas, x + t * .15f, y + t * .35f, t * .5f, t * .08f,
              RGB(159, 179, 170), 160u);
    return;
  }
  if (tile == 'i') {
    fill_rect(canvas, x, y, t, t, RGB(89, 166, 194), 256u);
    fill_rect(canvas, x + t * .12f, y + t * .18f, t * .65f, t * .05f,
              RGB(213, 243, 244), 256u);
    fill_rect(canvas, x + t * .32f, y + t * .65f, t * .55f, t * .05f,
              RGB(160, 220, 230), 256u);
    return;
  }
  stone_floor(canvas, x, y, t, seed);
  if (tile == '=') {
    fill_rect(canvas, x + t * .07f, y + t * .82f, t * .86f, t * .12f,
              RGB(234, 228, 206), 256u);
  } else if (tile == 'h') {
    fill_rect(canvas, x, y, t, t, RGB(146, 74, 58), 256u);
    fill_rect(canvas, x + t * .1f, y + t * .72f, t * .8f, t * .15f,
              RGB(244, 148, 74), 256u);
    for (int i = 0; i < 3; ++i)
      fill_rect(canvas, x + t * (.18f + .25f * (float)i), y + t * .16f, t * .1f,
                t * (.3f + .1f * (float)(i % 2)), RGB(242, 205, 173), 200u);
  } else if (tile == 'F') {
    fill_rect(canvas, x + t * .15f, y + t * .78f, t * .7f, t * .12f,
              RGB(74, 52, 47), 256u);
    triangle(canvas, x + t * .2f, y + t * .8f, x + t * .8f, y + t * .8f,
             x + t * .5f, y + t * .12f, RGB(232, 126, 63), 256u);
    fill_rect(canvas, x + t * .4f, y + t * .45f, t * .2f, t * .35f, C_GOLD,
              256u);
  } else if (tile == 'S' || tile == 'R') {
    fill_rect(canvas, x + t * .06f, y + t * .2f, t * .88f, t * .65f,
              RGB(223, 219, 201), 256u);
    fill_rect(canvas, x + t * .16f, y + t * .3f, t * .68f, t * .43f,
              RGB(64, 172, 190), 256u);
    if (tile == 'R')
      fill_rect(canvas, x + t * .5f, y + t * .3f, t * .34f, t * .43f,
                RGB(223, 123, 71), 256u);
  } else if (tile == 'm') {
    const bool taken = (game->facts & (ice ? H2_XIANXIA_RPG_FACT_C6_COLD
                                           : H2_XIANXIA_RPG_FACT_C6_HOT)) != 0u;
    fill_rect(canvas, x + t * .45f, y + t * .4f, t * .1f, t * .42f,
              RGB(42, 101, 81), 256u);
    if (!taken) {
      diamond(canvas, x + t * .5f, y + t * .3f, t * .22f,
              ice ? RGB(192, 243, 251) : RGB(246, 131, 77), 256u);
      fill_rect(canvas, x + t * .2f, y + t * .5f, t * .6f, t * .13f,
                RGB(103, 169, 115), 256u);
    }
  }
}

static void c7_tile_art(canvas_t* canvas, const game_t* game, char tile,
                        float x, float y, float t, uint32_t seed) {
  const bool past = (game->flags & H2_XIANXIA_RPG_FLAG_C7_PAST) != 0u;
  const bool route = tile == 'b' || tile == 'd' || tile == 'p';
  const bool open = h2_xianxia_rpg_c7_walkable(game, tile);
  const uint16_t water = past ? RGB(58, 94, 98) : RGB(27, 54, 77);
  fill_rect(canvas, x, y, t, t, water, 256u);
  if (tile == '~' || (route && !open)) {
    fill_rect(canvas, x + t * .15f, y + t * .4f, t * .6f, 2,
              past ? RGB(154, 164, 116) : RGB(78, 130, 154), 190u);
    if (tile == 'b') {
      fill_rect(canvas, x, y + t * .2f, t * .2f, t * .6f, RGB(95, 79, 67),
                256u);
      fill_rect(canvas, x + t * .8f, y + t * .3f, t * .2f, t * .5f,
                RGB(95, 79, 67), 256u);
    }
    return;
  }
  if (tile == '#') {
    fill_rect(canvas, x + 2, y + 2, t - 4, t - 4,
              past ? RGB(109, 89, 70) : RGB(53, 69, 80), 256u);
    fill_rect(canvas, x + 3, y + 3, t - 6, t * .12f,
              past ? RGB(186, 156, 101) : RGB(102, 136, 147), 256u);
    return;
  }
  noise_fill(canvas, x, y, t, past ? c3(148, 127, 89) : c3(73, 102, 113),
             past ? c3(133, 110, 80) : c3(65, 91, 104), .03f, seed);
  fill_rect(canvas, x, y + t - 1, t, 1,
            past ? RGB(104, 83, 62) : RGB(40, 67, 82), 256u);
  if (route) {
    for (int i = 1; i < 5; ++i)
      fill_rect(canvas, x, y + t * (float)i / 5, t, 2,
                past ? RGB(214, 184, 121) : RGB(139, 206, 211), 256u);
    return;
  }
  h2_xianxia_rpg_text_id_t label = H2_XIANXIA_RPG_TEXT_COUNT;
  if (tile == 'M') {
    diamond(canvas, x + t * .5f, y + t * .35f, t * .3f, RGB(213, 233, 224),
            256u);
    diamond(canvas, x + t * .5f, y + t * .35f, t * .21f, past ? C_GOLD : C_MP,
            256u);
    label = H2_XIANXIA_RPG_TEXT_C7_STONE;
  } else if (tile == 'L') {
    lamp_post(canvas, x, y - t * .2f, t,
              (game->facts & H2_XIANXIA_RPG_FACT_C7_LAMP) != 0u);
    label = H2_XIANXIA_RPG_TEXT_C7_LAMP;
  } else if (tile == 'W') {
    ellipse(canvas, x + t * .5f, y + t * .3f, t * .3f, t * .3f, 0, C_GOLD,
            256u);
    fill_rect(canvas, x + t * .46f, y + t * .02f, t * .08f, t * .56f, water,
              256u);
    fill_rect(canvas, x + t * .22f, y + t * .26f, t * .56f, t * .08f, water,
              256u);
    label = H2_XIANXIA_RPG_TEXT_C7_WATER;
  } else if (tile == 'A') {
    fill_rect(canvas, x + t * .2f, y + t * .05f, t * .6f, t * .5f,
              past ? C_GOLD : RGB(141, 169, 174), 256u);
    for (int i = 0; i < 3; ++i)
      fill_rect(canvas, x + t * .3f, y + t * (.15f + .12f * (float)i), t * .4f,
                2, water, 256u);
    label = H2_XIANXIA_RPG_TEXT_C7_TREATY;
  } else if (tile == 'H') {
    diamond(canvas, x + t * .5f, y + t * .3f, t * .27f, C_TEXT, 256u);
    label = H2_XIANXIA_RPG_TEXT_C7_HEART;
  } else if (tile == 'E') {
    label = H2_XIANXIA_RPG_TEXT_C7_EXIT;
  }
  if (label != H2_XIANXIA_RPG_TEXT_COUNT) {
    const char* word = h2_xianxia_rpg_text[label];
    const int32_t width = h2_xianxia_rpg_text_width(false, word);
    fill_rect(canvas, x + t * .5f - (float)width * .5f - 2, y + t * .55f,
              (float)width + 4, (float)LINE_PX, RGB(20, 33, 43), 256u);
    text_centered(canvas, false, word, x + t * .5f, (int32_t)(y + t * .55f),
                  C_TEXT, 256u);
  }
}

static void tile_art(canvas_t* canvas, const game_t* game, int32_t col,
                     int32_t row) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const h2_xianxia_rpg_map_t map = game->map;
  const float t = (float)l->tile;
  const float x = (float)(l->map_x + col * l->tile);
  const float y = (float)(l->map_y + row * l->tile);
  const uint32_t seed = hash32((uint32_t)(map * 977 + row * 31 + col));
  if (h2_xianxia_rpg_map_chapter(map) == 7u) {
    c7_tile_art(canvas, game, h2_xianxia_rpg_tile(map, col, row), x, y, t,
                seed);
    return;
  }
  if (h2_xianxia_rpg_map_chapter(map) == 6u) {
    c6_tile_art(canvas, game, h2_xianxia_rpg_tile(map, col, row), x, y, t,
                seed);
    return;
  }
  if (h2_xianxia_rpg_map_chapter(map) == 5u) {
    c5_tile_art(canvas, game, h2_xianxia_rpg_tile(map, col, row), x, y, t,
                seed);
    return;
  }
  if (h2_xianxia_rpg_map_chapter(map) == 4u &&
      map != H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE) {
    c4_tile_art(canvas, game, h2_xianxia_rpg_tile(map, col, row), x, y, t,
                seed);
    return;
  }
  if (map == H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE &&
      h2_xianxia_rpg_tile(map, col, row) == 'F') {
    noise_fill(canvas, x, y, t, c3(122, 112, 82), c3(92, 84, 68), 0.04f, seed);
    lamp_post(canvas, x, y, t, true);
    return;
  }
  if (h2_xianxia_rpg_map_chapter(map) == 3u) {
    c3_tile_art(canvas, game, h2_xianxia_rpg_tile(map, col, row), x, y, t,
                seed);
    return;
  }
  const bool chapter2 = h2_xianxia_rpg_map_chapter(map) == 2u;
  const bool returned = chapter2 ? game->stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN
                                 : game->stage >= H2_XIANXIA_RPG_STAGE_RETURN;
  const ground_t gr = ground_for(map, returned);
  const char tile = h2_xianxia_rpg_tile(map, col, row);
  const bool indoor = map == H2_XIANXIA_RPG_MAP_SHRINE;
  if (chapter2 && c2_tile_art(canvas, game, tile, &gr, x, y, t, seed, col, row))
    return;
  switch (tile) {
    case '.':
      grass(canvas, &gr, x, y, t, seed);
      break;
    case ',': {
      grass(canvas, &gr, x, y, t, seed);
      static const uint16_t petals[3] = {RGB(240, 160, 184), RGB(250, 246, 236),
                                         RGB(250, 214, 90)};
      for (uint32_t i = 0; i < 4u; ++i) {
        const float fx = x + t * (0.15f + 0.7f * hash_unit(seed, i + 20u));
        const float fy = y + t * (0.15f + 0.7f * hash_unit(seed, i + 30u));
        disc(canvas, fx, fy, t * 0.06f, petals[(seed + i) % 3u], 256u);
        disc(canvas, fx, fy, t * 0.025f, RGB(250, 220, 120), 256u);
      }
      break;
    }
    case '=':
      noise_fill(canvas, x, y, t, gr.path, scaled(gr.path, 0.8f), 0.05f, seed);
      for (uint32_t i = 0; i < 2u; ++i)
        ellipse(canvas, x + t * (0.2f + 0.6f * hash_unit(seed, i)),
                y + t * (0.2f + 0.6f * hash_unit(seed, i + 7u)), t * 0.06f,
                t * 0.04f, 0.0f, pack(scaled(gr.path, 0.75f)), 220u);
      break;
    case '~':
      water(canvas, &gr, x, y, t, seed);
      if ((map == H2_XIANXIA_RPG_MAP_VILLAGE ||
           map == H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE) &&
          returned &&
          (map != H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE ||
           game->stage >= H2_XIANXIA_RPG_STAGE_C4_RETURN) &&
          seed % 3u == 0u) {
        glow(canvas, x + t * 0.5f, y + t * 0.5f, t * 0.85f, RGB(255, 190, 90),
             170u);
        ellipse(canvas, x + t * 0.5f, y + t * 0.68f, t * 0.3f, t * 0.09f, 0.0f,
                RGB(150, 92, 52), 256u);
        triangle(canvas, x + t * 0.24f, y + t * 0.66f, x + t * 0.76f,
                 y + t * 0.66f, x + t * 0.5f, y + t * 0.26f, RGB(226, 72, 56),
                 256u);
        disc(canvas, x + t * 0.5f, y + t * 0.5f, t * 0.075f, RGB(255, 236, 150),
             256u);
      }
      break;
    case 'b':
      water(canvas, &gr, x, y, t, seed);
      fill_rect(canvas, x + t * 0.1f, y, t * 0.8f, t, RGB(150, 102, 60), 256u);
      for (int32_t i = 1; i < 5; ++i)
        fill_rect(canvas, x + t * 0.1f, y + t * 0.2f * (float)i, t * 0.8f, 1.0f,
                  RGB(96, 62, 36), 256u);
      fill_rect(canvas, x + t * 0.08f, y, t * 0.07f, t, RGB(90, 56, 32), 256u);
      fill_rect(canvas, x + t * 0.85f, y, t * 0.07f, t, RGB(90, 56, 32), 256u);
      break;
    case 'B': {
      if (indoor)
        noise_fill(canvas, x, y, t, c3(44, 60, 46), c3(30, 44, 34), 0.05f,
                   seed);
      else
        noise_fill(canvas, x, y, t, gr.grass_dark, scaled(gr.grass_dark, 0.8f),
                   0.05f, seed);
      for (uint32_t i = 0; i < 3u; ++i) {
        const float sx = x + t * (0.2f + 0.3f * (float)i) +
                         t * 0.08f * (hash_unit(seed, i) - 0.5f);
        const uint16_t stalk = i == 1u ? RGB(126, 184, 92) : RGB(96, 156, 74);
        bamboo_stalk(canvas, sx, y - t * 0.1f, y + t * 0.95f, t * 0.09f, stalk,
                     256u);
        fill_rect(canvas, sx - t * 0.045f, y - t * 0.1f, t * 0.025f, t * 1.05f,
                  RGB(170, 214, 128), 140u);
      }
      bamboo_leaves(canvas, x + t * 0.5f, y + t * 0.25f, t * 0.28f,
                    RGB(52, 110, 56), 230u, seed);
      break;
    }
    case 'T':
      grass(canvas, &gr, x, y, t, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.86f, t * 0.36f, t * 0.1f, 0.0f,
              RGB(20, 40, 20), 110u);
      fill_rect(canvas, x + t * 0.42f, y + t * 0.5f, t * 0.16f, t * 0.4f,
                RGB(104, 70, 42), 256u);
      disc(canvas, x + t * 0.5f, y + t * 0.38f, t * 0.42f, RGB(40, 92, 50),
           256u);
      disc(canvas, x + t * 0.32f, y + t * 0.3f, t * 0.24f, RGB(56, 116, 60),
           256u);
      disc(canvas, x + t * 0.66f, y + t * 0.26f, t * 0.2f, RGB(68, 132, 66),
           256u);
      disc(canvas, x + t * 0.4f, y + t * 0.2f, t * 0.1f, RGB(110, 170, 90),
           200u);
      break;
    case 'r':
      grass(canvas, &gr, x, y, t, seed);
      ellipse(canvas, x + t * 0.52f, y + t * 0.72f, t * 0.4f, t * 0.12f, 0.0f,
              RGB(20, 40, 20), 100u);
      ellipse(canvas, x + t * 0.5f, y + t * 0.55f, t * 0.36f, t * 0.28f, 0.0f,
              RGB(120, 120, 116), 256u);
      ellipse(canvas, x + t * 0.42f, y + t * 0.46f, t * 0.16f, t * 0.1f, -0.3f,
              RGB(170, 170, 164), 200u);
      break;
    case 'R': {
      const bool left = h2_xianxia_rpg_tile(map, col - 1, row) != 'R';
      const bool right = h2_xianxia_rpg_tile(map, col + 1, row) != 'R';
      grass(canvas, &gr, x, y, t, seed);
      const float x0 = left ? x + t * 0.06f : x;
      const float x1 = right ? x + t * 0.94f : x + t;
      fill_rect(canvas, x0, y + t * 0.18f, x1 - x0, t * 0.82f, RGB(64, 70, 86),
                256u);
      for (int32_t i = 0; i < 4; ++i)
        fill_rect(canvas, x0, y + t * (0.36f + 0.18f * (float)i), x1 - x0,
                  fmaxf(1.0f, t * 0.04f), RGB(42, 46, 60), 256u);
      for (int32_t i = 0; i < 5; ++i)
        fill_rect(canvas, x + t * 0.2f * (float)i + t * 0.08f, y + t * 0.2f,
                  fmaxf(1.0f, t * 0.03f), t * 0.8f, RGB(90, 98, 118), 140u);
      fill_rect(canvas, x0, y + t * 0.1f, x1 - x0, t * 0.14f, RGB(36, 36, 44),
                256u);
      if (left)
        triangle(canvas, x, y + t * 0.02f, x + t * 0.2f, y + t * 0.12f,
                 x + t * 0.1f, y + t * 0.3f, RGB(36, 36, 44), 256u);
      if (right)
        triangle(canvas, x + t, y + t * 0.02f, x + t * 0.8f, y + t * 0.12f,
                 x + t * 0.9f, y + t * 0.3f, RGB(36, 36, 44), 256u);
      fill_rect(canvas, x0, y + t * 0.94f, x1 - x0, t * 0.06f, RGB(20, 20, 26),
                200u);
      break;
    }
    case 'H':
    case 'D':
      fill_rect(canvas, x, y, t, t, RGB(228, 220, 202), 256u);
      fill_rect(canvas, x, y, t, t * 0.1f, RGB(90, 58, 40), 256u);
      fill_rect(canvas, x, y + t * 0.9f, t, t * 0.1f, RGB(120, 110, 100), 256u);
      if (h2_xianxia_rpg_tile(map, col - 1, row) != 'H' &&
          h2_xianxia_rpg_tile(map, col - 1, row) != 'D')
        fill_rect(canvas, x, y, t * 0.1f, t, RGB(90, 58, 40), 256u);
      if (h2_xianxia_rpg_tile(map, col + 1, row) != 'H' &&
          h2_xianxia_rpg_tile(map, col + 1, row) != 'D')
        fill_rect(canvas, x + t * 0.9f, y, t * 0.1f, t, RGB(90, 58, 40), 256u);
      if (tile == 'H') {
        fill_rect(canvas, x + t * 0.28f, y + t * 0.26f, t * 0.44f, t * 0.4f,
                  RGB(96, 64, 44), 256u);
        fill_rect(canvas, x + t * 0.32f, y + t * 0.3f, t * 0.36f, t * 0.32f,
                  RGB(238, 220, 180), 256u);
        fill_rect(canvas, x + t * 0.49f, y + t * 0.3f, fmaxf(1.0f, t * 0.03f),
                  t * 0.32f, RGB(96, 64, 44), 256u);
        fill_rect(canvas, x + t * 0.32f, y + t * 0.45f, t * 0.36f,
                  fmaxf(1.0f, t * 0.03f), RGB(96, 64, 44), 256u);
      } else {
        fill_rect(canvas, x + t * 0.2f, y + t * 0.16f, t * 0.6f, t * 0.84f,
                  RGB(84, 40, 30), 256u);
        fill_rect(canvas, x + t * 0.25f, y + t * 0.22f, t * 0.24f, t * 0.78f,
                  RGB(150, 56, 42), 256u);
        fill_rect(canvas, x + t * 0.51f, y + t * 0.22f, t * 0.24f, t * 0.78f,
                  RGB(150, 56, 42), 256u);
        disc(canvas, x + t * 0.45f, y + t * 0.6f, t * 0.03f, RGB(230, 190, 90),
             256u);
        disc(canvas, x + t * 0.55f, y + t * 0.6f, t * 0.03f, RGB(230, 190, 90),
             256u);
      }
      break;
    case 'W':
      noise_fill(canvas, x, y, t, gr.path, scaled(gr.path, 0.8f), 0.05f, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.8f, t * 0.42f, t * 0.12f, 0.0f,
              RGB(30, 30, 30), 90u);
      disc(canvas, x + t * 0.5f, y + t * 0.58f, t * 0.36f, RGB(130, 126, 118),
           256u);
      disc(canvas, x + t * 0.5f, y + t * 0.58f, t * 0.24f, RGB(24, 40, 54),
           256u);
      disc(canvas, x + t * 0.44f, y + t * 0.54f, t * 0.06f, RGB(120, 170, 200),
           160u);
      capsule(canvas, x + t * 0.16f, y + t * 0.62f, x + t * 0.16f,
              y + t * 0.12f, t * 0.04f, RGB(100, 64, 38), 256u);
      capsule(canvas, x + t * 0.84f, y + t * 0.62f, x + t * 0.84f,
              y + t * 0.12f, t * 0.04f, RGB(100, 64, 38), 256u);
      capsule(canvas, x + t * 0.1f, y + t * 0.12f, x + t * 0.9f, y + t * 0.12f,
              t * 0.05f, RGB(70, 44, 28), 256u);
      break;
    case 'f':
      grass(canvas, &gr, x, y, t, seed);
      for (int32_t i = 0; i < 2; ++i)
        fill_rect(canvas, x + t * (0.2f + 0.5f * (float)i), y + t * 0.3f,
                  t * 0.1f, t * 0.6f, RGB(140, 98, 58), 256u);
      fill_rect(canvas, x, y + t * 0.42f, t, t * 0.07f, RGB(120, 82, 48), 256u);
      fill_rect(canvas, x, y + t * 0.66f, t, t * 0.07f, RGB(120, 82, 48), 256u);
      break;
    case 'L':
      if (indoor)
        stone_floor(canvas, x, y, t, seed);
      else
        grass(canvas, &gr, x, y, t, seed);
      if (indoor || returned)
        glow(canvas, x + t * 0.5f, y + t * 0.36f, t * 0.6f, RGB(255, 190, 90),
             110u);
      fill_rect(canvas, x + t * 0.46f, y + t * 0.4f, t * 0.08f, t * 0.56f,
                RGB(60, 44, 36), 256u);
      ellipse(canvas, x + t * 0.5f, y + t * 0.34f, t * 0.18f, t * 0.22f, 0.0f,
              indoor || returned ? RGB(210, 52, 40) : RGB(104, 46, 40), 256u);
      fill_rect(canvas, x + t * 0.38f, y + t * 0.12f, t * 0.24f, t * 0.05f,
                RGB(214, 170, 70), 256u);
      fill_rect(canvas, x + t * 0.38f, y + t * 0.53f, t * 0.24f, t * 0.05f,
                RGB(214, 170, 70), 256u);
      ellipse(canvas, x + t * 0.46f, y + t * 0.3f, t * 0.05f, t * 0.1f, 0.0f,
              RGB(255, 170, 120), indoor || returned ? 180u : 0u);
      break;
    case '#': {
      noise_fill(canvas, x, y, t, c3(78, 72, 88), c3(60, 56, 70), 0.04f, seed);
      const float brick = t / 3.0f;
      for (int32_t i = 0; i < 3; ++i) {
        fill_rect(canvas, x, y + brick * (float)i, t, 1.0f, RGB(44, 40, 52),
                  256u);
        const float offset = (i % 2) ? brick * 0.5f : 0.0f;
        for (float bx = x + offset; bx < x + t; bx += brick * 1.5f)
          fill_rect(canvas, bx, y + brick * (float)i, 1.0f, brick,
                    RGB(44, 40, 52), 256u);
      }
      if (h2_xianxia_rpg_tile(map, col, row + 1) != '#')
        fill_rect(canvas, x, y + t * 0.86f, t, t * 0.14f, RGB(30, 26, 36),
                  220u);
      break;
    }
    case '_':
      stone_floor(canvas, x, y, t, seed);
      break;
    case 's':
      stone_floor(canvas, x, y, t, seed);
      for (int32_t i = 0; i < 3; ++i) {
        fill_rect(canvas, x, y + t * (0.1f + 0.3f * (float)i), t, t * 0.12f,
                  RGB(160, 154, 162), 230u);
        fill_rect(canvas, x, y + t * (0.22f + 0.3f * (float)i), t, t * 0.05f,
                  RGB(70, 66, 76), 230u);
      }
      break;
    case 'P':
      stone_floor(canvas, x, y, t, seed);
      ellipse(canvas, x + t * 0.5f, y + t * 0.9f, t * 0.32f, t * 0.08f, 0.0f,
              RGB(20, 16, 24), 120u);
      fill_rect(canvas, x + t * 0.28f, y - t * 0.15f, t * 0.44f, t * 1.0f,
                RGB(164, 40, 34), 256u);
      fill_rect(canvas, x + t * 0.3f, y - t * 0.15f, t * 0.1f, t * 1.0f,
                RGB(206, 86, 70), 200u);
      fill_rect(canvas, x + t * 0.22f, y + t * 0.78f, t * 0.56f, t * 0.12f,
                RGB(196, 156, 74), 256u);
      fill_rect(canvas, x + t * 0.22f, y - t * 0.18f, t * 0.56f, t * 0.1f,
                RGB(196, 156, 74), 256u);
      break;
    case 'A':
      stone_floor(canvas, x, y, t, seed);
      glow(canvas, x + t * 0.5f, y + t * 0.4f, t * 0.9f,
           returned ? RGB(255, 210, 110) : RGB(170, 110, 255), 120u);
      fill_rect(canvas, x + t * 0.1f, y + t * 0.42f, t * 0.8f, t * 0.5f,
                RGB(84, 78, 94), 256u);
      fill_rect(canvas, x + t * 0.06f, y + t * 0.36f, t * 0.88f, t * 0.1f,
                RGB(120, 112, 130), 256u);
      ellipse(canvas, x + t * 0.5f, y + t * 0.3f, t * 0.16f, t * 0.1f, 0.0f,
              RGB(170, 132, 60), 256u);
      capsule(canvas, x + t * 0.5f, y + t * 0.24f, x + t * 0.5f, y + t * 0.02f,
              t * 0.02f, RGB(220, 200, 230), 160u);
      break;
    case 'S':
      stone_floor(canvas, x, y, t, seed);
      glow(canvas, x + t * 0.5f, y + t * 0.5f, t * 0.8f, RGB(120, 240, 230),
           120u);
      disc(canvas, x + t * 0.5f, y + t * 0.55f, t * 0.4f, RGB(110, 104, 112),
           256u);
      disc(canvas, x + t * 0.5f, y + t * 0.55f, t * 0.32f, RGB(60, 190, 200),
           256u);
      disc(canvas, x + t * 0.42f, y + t * 0.48f, t * 0.1f, RGB(200, 255, 250),
           180u);
      break;
    case 'X':
      noise_fill(canvas, x, y, t, gr.path, scaled(gr.path, 0.8f), 0.05f, seed);
      if (game->stage < H2_XIANXIA_RPG_STAGE_TO_SHRINE) {
        fill_rect(canvas, x, y, t, t, RGB(120, 230, 220), 110u);
        for (int32_t i = 0; i < 4; ++i)
          fill_rect(canvas, x + t * (0.12f + 0.24f * (float)i), y, t * 0.04f, t,
                    RGB(210, 255, 250), 150u);
        fill_rect(canvas, x + t * 0.36f, y + t * 0.18f, t * 0.28f, t * 0.6f,
                  RGB(240, 214, 120), 256u);
        fill_rect(canvas, x + t * 0.45f, y + t * 0.26f, t * 0.1f, t * 0.44f,
                  RGB(196, 44, 40), 230u);
      }
      break;
    default:
      fill_rect(canvas, x, y, t, t, RGB(40, 40, 40), 256u);
      break;
  }
}

/* Chevrons on exit cells show where the path continues. */
static void exit_marks(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float t = (float)l->tile;
  for (int32_t row = 0; row < ROWS; ++row)
    for (int32_t col = 0; col < COLS; ++col) {
      h2_xianxia_rpg_map_t to;
      int32_t tx, ty;
      if (!h2_xianxia_rpg_exit_at(game->map, col, row, &to, &tx, &ty)) continue;
      const float cx = (float)(l->map_x + col * l->tile) + t * 0.5f;
      const float cy = (float)(l->map_y + row * l->tile) + t * 0.5f;
      float dx = 0.0f, dy = 0.0f;
      if (col == 0) dx = -1.0f;
      if (col == COLS - 1) dx = 1.0f;
      if (row == 0) dy = -1.0f;
      if (row == ROWS - 1) dy = 1.0f;
      for (int32_t i = 0; i < 2; ++i) {
        const float ox = cx + dx * t * (0.1f * (float)i - 0.05f);
        const float oy = cy + dy * t * (0.1f * (float)i - 0.05f);
        const float px = -dy, py = dx;
        capsule(canvas, ox - px * t * 0.18f - dx * t * 0.12f,
                oy - py * t * 0.18f - dy * t * 0.12f, ox, oy, t * 0.035f,
                RGB(255, 240, 200), 200u);
        capsule(canvas, ox + px * t * 0.18f - dx * t * 0.12f,
                oy + py * t * 0.18f - dy * t * 0.12f, ox, oy, t * 0.035f,
                RGB(255, 240, 200), 200u);
      }
    }
}

static uint32_t fog_alpha(const game_t* game) {
  const uint32_t flags = game->flags;
  if (game->stage >= H2_XIANXIA_RPG_STAGE_C2_RETURN) return 0u;
  switch (game->map) {
    case H2_XIANXIA_RPG_MAP_DOCK:
      return 64u;
    case H2_XIANXIA_RPG_MAP_REEDS: {
      uint32_t lit = 0;
      if (flags & H2_XIANXIA_RPG_FLAG_C2_POST1) ++lit;
      if (flags & H2_XIANXIA_RPG_FLAG_C2_POST2) ++lit;
      if (flags & H2_XIANXIA_RPG_FLAG_C2_BOAT) ++lit;
      return 96u - 22u * lit;
    }
    case H2_XIANXIA_RPG_MAP_WRECK:
      return 72u;
    default:
      return 0u;
  }
}

/* Chapter 2 lies under river fog that thins as lamps are lit; every lit
 * lamp opens a warm circle in it. */
static void river_fog(canvas_t* canvas, const game_t* game) {
  const uint32_t alpha = fog_alpha(game);
  if (alpha == 0u) return;
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float t = (float)l->tile;
  fill_rect(canvas, (float)l->map_x, (float)l->map_y, (float)(COLS * l->tile),
            (float)(ROWS * l->tile), RGB(218, 224, 228), alpha);
  for (uint32_t i = 0; i < 6u; ++i)
    glow(canvas, (float)l->map_x + (float)(COLS * l->tile) * hash_unit(91u, i),
         (float)l->map_y + (float)(ROWS * l->tile) * hash_unit(97u, i),
         t * 2.4f, RGB(240, 244, 246), alpha);
  for (int32_t row = 0; row < ROWS; ++row)
    for (int32_t col = 0; col < COLS; ++col) {
      const char tile = h2_xianxia_rpg_tile(game->map, col, row);
      bool lit = false;
      switch (tile) {
        case '1':
          lit = (game->flags & H2_XIANXIA_RPG_FLAG_C2_POST1) != 0u;
          break;
        case '2':
          lit = (game->flags & H2_XIANXIA_RPG_FLAG_C2_POST2) != 0u;
          break;
        case '4':
          lit = (game->flags & H2_XIANXIA_RPG_FLAG_C2_POST4) != 0u;
          break;
        case 'U':
          lit = game->map == H2_XIANXIA_RPG_MAP_WRECK ||
                (game->flags & H2_XIANXIA_RPG_FLAG_C2_BOAT) != 0u;
          break;
        default:
          break;
      }
      if (!lit) continue;
      const float cx = (float)(l->map_x + col * l->tile) + t * 0.5f;
      const float cy = (float)(l->map_y + row * l->tile) + t * 0.3f;
      glow(canvas, cx, cy, t * 2.6f, RGB(255, 236, 190), 80u);
      glow(canvas, cx, cy, t * 1.2f, RGB(220, 255, 240), 90u);
    }
}

static void map_background(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float w = (float)canvas->width, h = (float)canvas->height;
  gradient_rect(canvas, 0.0f, 0.0f, w, h, c3(30, 26, 34), c3(18, 16, 22));
  for (int32_t row = 0; row < ROWS; ++row)
    for (int32_t col = 0; col < COLS; ++col) tile_art(canvas, game, col, row);
  if (game->chapter == 4u && game->stage < H2_XIANXIA_RPG_STAGE_C4_RETURN) {
    fill_rect(canvas, (float)l->map_x, (float)l->map_y, (float)(COLS * l->tile),
              (float)(ROWS * l->tile), RGB(14, 22, 46),
              game->map == H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE ? 110u : 48u);
  }
  river_fog(canvas, game);
  exit_marks(canvas, game);
  /* Soft vignette at the map rim. */
  const float mw = (float)(COLS * l->tile), mh = (float)(ROWS * l->tile);
  const float mx = (float)l->map_x, my = (float)l->map_y;
  const float edge = (float)l->tile * 0.4f;
  for (int32_t i = 0; i < 4; ++i) {
    const float k = (float)i / 4.0f;
    const uint32_t alpha = (uint32_t)(70.0f * (1.0f - k));
    fill_rect(canvas, mx, my + edge * k, mw, 1.0f, C_SHADOW, alpha);
    fill_rect(canvas, mx, my + mh - 1.0f - edge * k, mw, 1.0f, C_SHADOW, alpha);
    fill_rect(canvas, mx + edge * k, my, 1.0f, mh, C_SHADOW, alpha);
    fill_rect(canvas, mx + mw - 1.0f - edge * k, my, 1.0f, mh, C_SHADOW, alpha);
  }
}

/* ---- Characters -------------------------------------------------------- */

typedef struct look {
  uint16_t robe, trim, hair, skin;
  float scale;
} look_t;

static look_t look_of(h2_xianxia_rpg_actor_t actor) {
  const uint16_t skin = RGB(246, 214, 180);
  switch (actor) {
    case H2_XIANXIA_RPG_ACTOR_XUN:
      return (look_t){RGB(117, 69, 101), RGB(220, 204, 157), RGB(185, 189, 178),
                      skin, .95f};
    case H2_XIANXIA_RPG_ACTOR_WEN_GRANDMA:
      return (look_t){RGB(122, 74, 64), RGB(224, 198, 151), RGB(206, 208, 196),
                      skin, 0.9f};
    case H2_XIANXIA_RPG_ACTOR_BOAT_DOCTOR:
      return (look_t){RGB(80, 118, 96), RGB(226, 218, 174), RGB(122, 126, 118),
                      skin, 0.95f};
    case H2_XIANXIA_RPG_ACTOR_SHIYAN:
      return (look_t){RGB(90, 102, 112), RGB(228, 204, 160), RGB(210, 214, 210),
                      skin, 0.95f};
    case H2_XIANXIA_RPG_ACTOR_WENZHOU:
      return (look_t){RGB(56, 66, 84), RGB(158, 126, 84), RGB(30, 36, 42), skin,
                      1.0f};
    case H2_XIANXIA_RPG_ACTOR_SWORD_SOUL:
      return (look_t){RGB(92, 146, 162), RGB(224, 246, 242), RGB(60, 92, 110),
                      RGB(186, 222, 230), 1.08f};
    case H2_XIANXIA_RPG_ACTOR_ELDER:
      return (look_t){RGB(130, 96, 62), RGB(220, 200, 150), RGB(200, 200, 196),
                      skin, 0.95f};
    case H2_XIANXIA_RPG_ACTOR_HERBALIST:
      return (look_t){RGB(122, 96, 136), RGB(226, 214, 190), RGB(186, 184, 186),
                      skin, 0.9f};
    case H2_XIANXIA_RPG_ACTOR_CHILD:
      return (look_t){RGB(206, 66, 56), RGB(250, 220, 120), RGB(36, 30, 32),
                      skin, 0.75f};
    case H2_XIANXIA_RPG_ACTOR_DAOIST:
      return (look_t){RGB(236, 236, 228), RGB(130, 140, 150),
                      RGB(170, 170, 170), skin, 1.0f};
    case H2_XIANXIA_RPG_ACTOR_JIANGLI:
      return (look_t){RGB(46, 128, 136), RGB(232, 240, 230), RGB(28, 26, 32),
                      skin, 0.92f};
    case H2_XIANXIA_RPG_ACTOR_FERRYMASTER:
      return (look_t){RGB(104, 80, 58), RGB(210, 190, 146), RGB(60, 52, 48),
                      RGB(232, 196, 160), 1.0f};
    case H2_XIANXIA_RPG_ACTOR_TEAWIFE:
      return (look_t){RGB(156, 92, 78), RGB(244, 236, 220), RGB(64, 56, 60),
                      skin, 0.9f};
    case H2_XIANXIA_RPG_ACTOR_TRAVELER:
      return (look_t){RGB(86, 104, 146), RGB(220, 210, 180), RGB(40, 34, 36),
                      skin, 0.95f};
    case H2_XIANXIA_RPG_ACTOR_FISHER:
      return (look_t){RGB(96, 116, 96), RGB(206, 196, 150), RGB(210, 210, 206),
                      RGB(232, 196, 160), 0.9f};
    default:
      return (look_t){RGB(60, 124, 176), RGB(226, 236, 244), RGB(30, 28, 34),
                      skin, 1.0f};
  }
}

/* A small chibi figure standing on (cx, feet). t is the tile size. */
static void draw_person(canvas_t* canvas, h2_xianxia_rpg_actor_t actor,
                        float cx, float feet, float t,
                        h2_xianxia_rpg_facing_t facing, float walk, float bob) {
  const look_t k = look_of(actor);
  const float s = t * k.scale;
  const float body_top = feet - s * 0.62f + bob;
  const float head_y = feet - s * 0.8f + bob;
  const float head_r = s * 0.22f;
  const float side = facing == H2_XIANXIA_RPG_FACING_LEFT    ? -1.0f
                     : facing == H2_XIANXIA_RPG_FACING_RIGHT ? 1.0f
                                                             : 0.0f;
  ellipse(canvas, cx, feet - s * 0.03f, s * 0.28f, s * 0.08f, 0.0f, C_SHADOW,
          110u);
  /* Feet alternate while walking. */
  const float stride = sinf(walk) * s * 0.08f;
  ellipse(canvas, cx - s * 0.1f + (side != 0.0f ? stride : 0.0f),
          feet - s * 0.06f + (side == 0.0f ? stride * 0.5f : 0.0f), s * 0.08f,
          s * 0.05f, 0.0f, RGB(40, 32, 30), 256u);
  ellipse(canvas, cx + s * 0.1f - (side != 0.0f ? stride : 0.0f),
          feet - s * 0.06f - (side == 0.0f ? stride * 0.5f : 0.0f), s * 0.08f,
          s * 0.05f, 0.0f, RGB(40, 32, 30), 256u);
  /* Sword on the hero's back, behind the robe. */
  if (actor == H2_XIANXIA_RPG_ACTOR_HERO && facing != H2_XIANXIA_RPG_FACING_UP)
    capsule(canvas, cx - s * 0.2f, body_top - s * 0.06f, cx + s * 0.22f,
            body_top + s * 0.36f, s * 0.035f, RGB(200, 206, 214), 256u);
  /* Robe: a flared trapezoid with a trim band at the hem. */
  quad(canvas, cx - s * 0.18f, body_top, cx + s * 0.18f, body_top,
       cx + s * 0.28f, feet - s * 0.08f, cx - s * 0.28f, feet - s * 0.08f,
       k.robe, 256u);
  fill_rect(canvas, cx - s * 0.27f, feet - s * 0.16f, s * 0.54f, s * 0.07f,
            k.trim, 220u);
  fill_rect(canvas, cx - s * 0.2f, body_top + s * 0.2f, s * 0.4f, s * 0.05f,
            RGB(60, 40, 30), 230u);
  if (facing == H2_XIANXIA_RPG_FACING_DOWN)
    triangle(canvas, cx - s * 0.1f, body_top, cx + s * 0.1f, body_top, cx,
             body_top + s * 0.16f, k.trim, 256u);
  /* Sleeves. */
  const float swing = sinf(walk) * s * 0.05f;
  capsule(canvas, cx - s * 0.2f, body_top + s * 0.06f, cx - s * 0.26f,
          body_top + s * 0.3f + swing, s * 0.07f, k.robe, 256u);
  capsule(canvas, cx + s * 0.2f, body_top + s * 0.06f, cx + s * 0.26f,
          body_top + s * 0.3f - swing, s * 0.07f, k.robe, 256u);
  disc(canvas, cx - s * 0.26f, body_top + s * 0.34f + swing, s * 0.045f, k.skin,
       256u);
  disc(canvas, cx + s * 0.26f, body_top + s * 0.34f - swing, s * 0.045f, k.skin,
       256u);
  /* Head, hair and face. */
  disc(canvas, cx, head_y, head_r, k.skin, 256u);
  if (facing == H2_XIANXIA_RPG_FACING_UP) {
    disc(canvas, cx, head_y, head_r * 1.02f, k.hair, 256u);
  } else {
    ellipse(canvas, cx - side * head_r * 0.3f, head_y - head_r * 0.45f,
            head_r * 1.02f, head_r * 0.62f, 0.0f, k.hair, 256u);
    if (side == 0.0f) {
      disc(canvas, cx - head_r * 0.38f, head_y + head_r * 0.1f,
           fmaxf(1.0f, head_r * 0.12f), RGB(30, 24, 28), 256u);
      disc(canvas, cx + head_r * 0.38f, head_y + head_r * 0.1f,
           fmaxf(1.0f, head_r * 0.12f), RGB(30, 24, 28), 256u);
      disc(canvas, cx - head_r * 0.55f, head_y + head_r * 0.42f, head_r * 0.14f,
           RGB(240, 150, 140), 120u);
      disc(canvas, cx + head_r * 0.55f, head_y + head_r * 0.42f, head_r * 0.14f,
           RGB(240, 150, 140), 120u);
    } else {
      disc(canvas, cx + side * head_r * 0.45f, head_y + head_r * 0.1f,
           fmaxf(1.0f, head_r * 0.12f), RGB(30, 24, 28), 256u);
    }
  }
  switch (actor) {
    case H2_XIANXIA_RPG_ACTOR_HERO:
      disc(canvas, cx, head_y - head_r * 1.05f, head_r * 0.36f, k.hair, 256u);
      capsule(canvas, cx + head_r * 0.2f, head_y - head_r * 1.05f,
              cx + head_r * 0.75f, head_y - head_r * 0.6f, head_r * 0.08f,
              RGB(90, 180, 230), 256u);
      break;
    case H2_XIANXIA_RPG_ACTOR_ELDER:
      if (facing != H2_XIANXIA_RPG_FACING_UP)
        ellipse(canvas, cx + side * head_r * 0.2f, head_y + head_r * 0.85f,
                head_r * 0.5f, head_r * 0.6f, 0.0f, RGB(244, 244, 240), 256u);
      capsule(canvas, cx + s * 0.34f, feet - s * 0.05f, cx + s * 0.3f,
              head_y - head_r * 0.6f, s * 0.035f, RGB(120, 84, 50), 256u);
      break;
    case H2_XIANXIA_RPG_ACTOR_HERBALIST:
      disc(canvas, cx, head_y - head_r * 0.95f, head_r * 0.4f, k.hair, 256u);
      fill_rect(canvas, cx - s * 0.06f, head_y - head_r * 1.0f, s * 0.12f,
                s * 0.02f, RGB(60, 120, 70), 256u);
      ellipse(canvas, cx - s * 0.32f, body_top + s * 0.4f, s * 0.12f, s * 0.09f,
              0.0f, RGB(160, 120, 70), 256u);
      ellipse(canvas, cx - s * 0.32f, body_top + s * 0.36f, s * 0.09f,
              s * 0.04f, 0.0f, RGB(90, 150, 80), 256u);
      break;
    case H2_XIANXIA_RPG_ACTOR_CHILD:
      disc(canvas, cx - head_r * 0.8f, head_y - head_r * 0.8f, head_r * 0.34f,
           k.hair, 256u);
      disc(canvas, cx + head_r * 0.8f, head_y - head_r * 0.8f, head_r * 0.34f,
           k.hair, 256u);
      break;
    case H2_XIANXIA_RPG_ACTOR_JIANGLI: {
      /* Side braid, headband, a punting pole and the lantern at her waist. */
      disc(canvas, cx, head_y - head_r * 1.0f, head_r * 0.32f, k.hair, 256u);
      fill_rect(canvas, cx - head_r * 0.9f, head_y - head_r * 0.62f,
                head_r * 1.8f, head_r * 0.18f, RGB(60, 170, 170), 230u);
      if (facing != H2_XIANXIA_RPG_FACING_UP)
        capsule(canvas, cx + head_r * 0.8f, head_y, cx + head_r * 0.9f,
                body_top + s * 0.2f, head_r * 0.16f, k.hair, 256u);
      capsule(canvas, cx + s * 0.34f, feet - s * 0.02f, cx + s * 0.22f,
              head_y - head_r * 2.0f, s * 0.028f, RGB(170, 150, 90), 256u);
      glow(canvas, cx - s * 0.26f, body_top + s * 0.42f, s * 0.22f,
           RGB(200, 255, 236), 170u);
      rounded_rect(canvas, cx - s * 0.32f, body_top + s * 0.34f, cx - s * 0.2f,
                   body_top + s * 0.5f, s * 0.03f, RGB(236, 255, 240), 256u);
      break;
    }
    case H2_XIANXIA_RPG_ACTOR_FERRYMASTER:
    case H2_XIANXIA_RPG_ACTOR_FISHER: {
      /* Bamboo rain hat; the fisher also has a beard and a rod. */
      const float brim =
          head_r * (actor == H2_XIANXIA_RPG_ACTOR_FISHER ? 1.45f : 1.75f);
      ellipse(canvas, cx, head_y - head_r * 0.55f, brim, head_r * 0.36f, 0.0f,
              RGB(196, 170, 112), 256u);
      triangle(canvas, cx - brim * 0.6f, head_y - head_r * 0.6f,
               cx + brim * 0.6f, head_y - head_r * 0.6f, cx,
               head_y - head_r * 1.5f, RGB(212, 186, 124), 256u);
      capsule(canvas, cx - brim * 0.9f, head_y - head_r * 0.52f,
              cx + brim * 0.9f, head_y - head_r * 0.52f, head_r * 0.06f,
              RGB(140, 116, 70), 230u);
      if (actor == H2_XIANXIA_RPG_ACTOR_FISHER) {
        if (facing != H2_XIANXIA_RPG_FACING_UP)
          ellipse(canvas, cx + side * head_r * 0.2f, head_y + head_r * 0.85f,
                  head_r * 0.45f, head_r * 0.55f, 0.0f, RGB(240, 240, 236),
                  256u);
        capsule(canvas, cx - s * 0.3f, body_top + s * 0.34f, cx - s * 0.62f,
                head_y - head_r * 2.2f, s * 0.018f, RGB(120, 92, 56), 256u);
      } else {
        fill_rect(canvas, cx - s * 0.2f, body_top + s * 0.16f, s * 0.4f,
                  s * 0.06f, RGB(200, 60, 44), 240u);
      }
      break;
    }
    case H2_XIANXIA_RPG_ACTOR_TEAWIFE:
      disc(canvas, cx, head_y - head_r * 0.95f, head_r * 0.42f, k.hair, 256u);
      capsule(canvas, cx - head_r * 0.5f, head_y - head_r * 1.0f,
              cx + head_r * 0.5f, head_y - head_r * 1.1f, head_r * 0.06f,
              RGB(220, 180, 80), 256u);
      if (facing != H2_XIANXIA_RPG_FACING_UP)
        fill_rect(canvas, cx - s * 0.15f, body_top + s * 0.24f, s * 0.3f,
                  s * 0.3f, RGB(246, 240, 228), 230u);
      break;
    case H2_XIANXIA_RPG_ACTOR_TRAVELER:
      disc(canvas, cx, head_y - head_r * 1.0f, head_r * 0.3f, k.hair, 256u);
      if (facing != H2_XIANXIA_RPG_FACING_DOWN)
        disc(canvas, cx - side * s * 0.16f, body_top + s * 0.18f, s * 0.16f,
             RGB(150, 110, 70), 256u);
      capsule(canvas, cx - s * 0.18f, body_top + s * 0.02f, cx + s * 0.2f,
              body_top + s * 0.3f, s * 0.02f, RGB(120, 84, 50), 230u);
      break;
    case H2_XIANXIA_RPG_ACTOR_DAOIST:
      fill_rect(canvas, cx - head_r * 0.45f, head_y - head_r * 1.45f,
                head_r * 0.9f, head_r * 0.6f, RGB(30, 30, 36), 256u);
      if (facing != H2_XIANXIA_RPG_FACING_UP)
        ellipse(canvas, cx + side * head_r * 0.2f, head_y + head_r * 1.0f,
                head_r * 0.36f, head_r * 0.8f, 0.0f, RGB(230, 230, 230), 256u);
      capsule(canvas, cx - s * 0.3f, body_top + s * 0.32f, cx - s * 0.42f,
              body_top + s * 0.02f, s * 0.025f, RGB(120, 84, 50), 256u);
      ellipse(canvas, cx - s * 0.44f, body_top - s * 0.06f, s * 0.05f,
              s * 0.12f, 0.3f, RGB(250, 250, 246), 256u);
      break;
    default:
      break;
  }
}

/* The ink dragon coiled on the map, centered on (cx, cy). */
static void draw_map_dragon(canvas_t* canvas, float cx, float cy, float t,
                            uint64_t time_ms) {
  const float breathe = sinf((float)(time_ms % 4000u) * (2.0f * PI / 4000.0f));
  glow(canvas, cx, cy, t * 1.3f, RGB(80, 50, 130), 110u);
  ellipse(canvas, cx, cy + t * 0.4f, t * 0.8f, t * 0.2f, 0.0f, C_SHADOW, 120u);
  for (int32_t i = 0; i < 14; ++i) {
    const float a = (float)i * 0.55f;
    const float r = t * (0.62f - 0.03f * (float)i);
    const float x = cx + cosf(a) * r * 0.9f;
    const float y = cy + t * 0.15f + sinf(a) * r * 0.4f;
    disc(canvas, x, y, t * (0.22f - 0.008f * (float)i), RGB(40, 44, 62), 256u);
    disc(canvas, x, y - t * 0.06f, t * (0.1f - 0.004f * (float)i),
         RGB(80, 88, 116), 160u);
  }
  const float hx = cx - t * 0.1f, hy = cy - t * 0.45f + breathe * t * 0.05f;
  capsule(canvas, cx + t * 0.2f, cy, hx, hy, t * 0.18f, RGB(40, 44, 62), 256u);
  ellipse(canvas, hx, hy, t * 0.3f, t * 0.2f, -0.2f, RGB(48, 52, 74), 256u);
  capsule(canvas, hx + t * 0.1f, hy - t * 0.12f, hx + t * 0.3f, hy - t * 0.42f,
          t * 0.04f, RGB(226, 206, 150), 256u);
  capsule(canvas, hx - t * 0.1f, hy - t * 0.12f, hx - t * 0.2f, hy - t * 0.42f,
          t * 0.04f, RGB(226, 206, 150), 256u);
  disc(canvas, hx - t * 0.12f, hy, t * 0.05f, RGB(255, 206, 64), 256u);
  disc(canvas, hx + t * 0.08f, hy, t * 0.05f, RGB(255, 206, 64), 256u);
  capsule(canvas, hx - t * 0.26f, hy + t * 0.08f, hx - t * 0.6f,
          hy + t * 0.2f + breathe * t * 0.06f, t * 0.015f, RGB(200, 200, 210),
          200u);
}

/* The mist ghost blocking the wreck's hatch, standing on (cx, feet). */
static void draw_map_guard(canvas_t* canvas, float cx, float feet, float t,
                           uint64_t time_ms) {
  const float sway = sinf((float)(time_ms % 3000u) * (2.0f * PI / 3000.0f));
  glow(canvas, cx, feet - t * 0.5f, t * 1.0f, RGB(170, 190, 210), 130u);
  ellipse(canvas, cx, feet - t * 0.04f, t * 0.32f, t * 0.08f, 0.0f, C_SHADOW,
          110u);
  for (int32_t i = 0; i < 4; ++i) {
    const float k = (float)i / 3.0f;
    disc(canvas, cx + sway * t * 0.06f * k, feet - t * (0.2f + 0.22f * k),
         t * (0.3f - 0.06f * k), RGB(120, 136, 152), 220u);
  }
  rounded_rect(canvas, cx - t * 0.2f, feet - t * 0.92f, cx + t * 0.2f,
               feet - t * 0.68f, t * 0.06f, RGB(90, 100, 116), 256u);
  fill_rect(canvas, cx - t * 0.24f, feet - t * 0.74f, t * 0.48f, t * 0.06f,
            RGB(70, 78, 92), 256u);
  disc(canvas, cx - t * 0.08f, feet - t * 0.8f, t * 0.04f, RGB(150, 230, 255),
       256u);
  disc(canvas, cx + t * 0.08f, feet - t * 0.8f, t * 0.04f, RGB(150, 230, 255),
       256u);
  capsule(canvas, cx + t * 0.3f, feet - t * 0.1f, cx + t * 0.34f,
          feet - t * 0.86f, t * 0.03f, RGB(80, 86, 96), 256u);
  disc(canvas, cx + t * 0.34f, feet - t * 0.1f, t * 0.08f, RGB(80, 86, 96),
       256u);
}

/* ---- Battle sprites ---------------------------------------------------- */

/* The same three poses shown in the stele carvings: hanging rain blades,
 * a sword drawn back at the waist, and a horizontal counter guard. */
static void draw_sword_soul(canvas_t* canvas, float cx, float cy, float s,
                            uint8_t stance, uint64_t time_ms) {
  const float bob = sinf((float)(time_ms % 2400u) * PI / 1200.0f) * s * 0.02f;
  const float feet = cy + s * 0.65f + bob;
  draw_person(canvas, H2_XIANXIA_RPG_ACTOR_SWORD_SOUL, cx, feet, s * 1.12f,
              H2_XIANXIA_RPG_FACING_LEFT, 0, 0);
  const uint16_t edge = RGB(224, 246, 250);
  if (stance == 0u) {
    for (int i = 0; i < 3; ++i) {
      const float x = cx + s * (-0.5f + (float)i * 0.44f);
      capsule(canvas, x, cy - s * 0.38f, x, cy + s * 0.42f, s * 0.015f, edge,
              240u);
      triangle(canvas, x - s * 0.03f, cy + s * 0.35f, x + s * 0.03f,
               cy + s * 0.35f, x, cy + s * 0.51f, edge, 256u);
    }
  } else if (stance == 1u) {
    capsule(canvas, cx - s * 0.1f, cy + s * 0.08f, cx + s * 0.62f,
            cy - s * 0.06f, s * 0.025f, edge, 256u);
    capsule(canvas, cx - s * 0.64f, cy + s * 0.15f, cx - s * 0.25f,
            cy + s * 0.08f, s * 0.012f, C_GOLD, 230u);
  } else {
    capsule(canvas, cx - s * 0.58f, cy - s * 0.02f, cx + s * 0.58f,
            cy - s * 0.02f, s * 0.035f, edge, 256u);
    capsule(canvas, cx + s * 0.26f, cy - s * 0.16f, cx + s * 0.26f,
            cy + s * 0.12f, s * 0.025f, C_GOLD, 256u);
    ellipse(canvas, cx, feet - s * 0.12f, s * 0.55f, s * 0.08f, -0.1f, edge,
            100u);
  }
}

static void draw_enemy(canvas_t* canvas, h2_xianxia_rpg_enemy_t enemy, float cx,
                       float cy, float s, uint64_t time_ms, bool charging) {
  const h2_xianxia_rpg_enemy_info_t* info = h2_xianxia_rpg_enemy_info(enemy);
  const float phase = (float)(time_ms % 2400u) * (2.0f * PI / 2400.0f);
  const float bob = sinf(phase) * s * 0.04f;
  cy += bob;
  switch (enemy) {
    case H2_XIANXIA_RPG_ENEMY_MIRROR:
      ellipse(canvas, cx, cy + s * .55f, s * .5f, s * .08f, 0,
              RGB(154, 213, 224), 256u);
      draw_person(canvas, H2_XIANXIA_RPG_ACTOR_HERO, cx, cy + s * .5f, s,
                  H2_XIANXIA_RPG_FACING_LEFT, 0, bob);
      fill_rect(canvas, cx - s * .24f, cy - s * .5f, s * .48f, s,
                RGB(123, 196, 213), 70u);
      break;
    case H2_XIANXIA_RPG_ENEMY_WENZHOU:
      draw_person(canvas, H2_XIANXIA_RPG_ACTOR_WENZHOU, cx, cy + s * 0.5f, s,
                  H2_XIANXIA_RPG_FACING_LEFT, 0, bob);
      capsule(canvas, cx - s * 0.3f, cy + s * 0.1f, cx - s * 0.6f,
              cy - s * 0.6f, s * 0.025f, RGB(224, 237, 230), 256u);
      break;
    case H2_XIANXIA_RPG_ENEMY_SWORD_SOUL:
      draw_sword_soul(canvas, cx, cy, s, charging ? 1u : 0u, time_ms);
      break;
    case H2_XIANXIA_RPG_ENEMY_RAIN_CROW:
      ellipse(canvas, cx, cy, s * 0.24f, s * 0.18f, 0, info->body, 256u);
      triangle(canvas, cx - s * 0.1f, cy, cx - s * 0.7f, cy - s * 0.4f,
               cx - s * 0.35f, cy + s * 0.2f, info->body, 256u);
      triangle(canvas, cx + s * 0.1f, cy, cx + s * 0.7f, cy - s * 0.4f,
               cx + s * 0.35f, cy + s * 0.2f, info->body, 256u);
      triangle(canvas, cx - s * 0.2f, cy - s * 0.1f, cx - s * 0.42f, cy,
               cx - s * 0.16f, cy + s * 0.04f, info->accent, 256u);
      disc(canvas, cx - s * 0.12f, cy - s * 0.08f, s * 0.025f, C_GOLD, 256u);
      break;
    case H2_XIANXIA_RPG_ENEMY_BROKEN_SWORD:
      capsule(canvas, cx, cy + s * 0.4f, cx, cy - s * 0.5f, s * 0.07f,
              info->body, 256u);
      capsule(canvas, cx - s * 0.3f, cy + s * 0.18f, cx + s * 0.3f,
              cy + s * 0.18f, s * 0.04f, info->accent, 256u);
      capsule(canvas, cx - s * 0.08f, cy - s * 0.15f, cx + s * 0.08f,
              cy - s * 0.28f, s * 0.025f, RGB(24, 38, 52), 256u);
      break;
    case H2_XIANXIA_RPG_ENEMY_LEAF_SPRITE:
    case H2_XIANXIA_RPG_ENEMY_REED_SPRITE:
      ellipse(canvas, cx, cy + s * 0.55f, s * 0.36f, s * 0.08f, 0.0f, C_SHADOW,
              110u);
      for (int32_t i = 0; i < 7; ++i) {
        const float a = (float)i * (2.0f * PI / 7.0f) + phase * 0.2f;
        ellipse(canvas, cx + cosf(a) * s * 0.26f, cy + sinf(a) * s * 0.26f,
                s * 0.28f, s * 0.11f, a, i % 2 ? info->body : RGB(60, 130, 64),
                256u);
        capsule(canvas, cx + cosf(a) * s * 0.06f, cy + sinf(a) * s * 0.06f,
                cx + cosf(a) * s * 0.46f, cy + sinf(a) * s * 0.46f, s * 0.01f,
                RGB(170, 220, 120), 200u);
      }
      disc(canvas, cx, cy, s * 0.2f, info->accent, 256u);
      disc(canvas, cx - s * 0.07f, cy - s * 0.02f, s * 0.035f, RGB(30, 40, 20),
           256u);
      disc(canvas, cx + s * 0.07f, cy - s * 0.02f, s * 0.035f, RGB(30, 40, 20),
           256u);
      break;
    case H2_XIANXIA_RPG_ENEMY_CIVET:
    case H2_XIANXIA_RPG_ENEMY_TIDE_OTTER:
    case H2_XIANXIA_RPG_ENEMY_REED_OTTER: {
      ellipse(canvas, cx, cy + s * 0.42f, s * 0.42f, s * 0.08f, 0.0f, C_SHADOW,
              110u);
      for (int32_t i = 0; i < 6; ++i) {
        const float k = (float)i / 5.0f;
        disc(canvas, cx + s * (0.3f + 0.12f * k),
             cy - s * (0.05f + 0.4f * k * k) + sinf(phase + k) * s * 0.03f,
             s * (0.11f + 0.05f * sinf(k * PI)),
             i == 5 ? RGB(250, 240, 220) : info->body, 256u);
      }
      ellipse(canvas, cx, cy + s * 0.1f, s * 0.34f, s * 0.22f, 0.0f, info->body,
              256u);
      ellipse(canvas, cx - s * 0.04f, cy + s * 0.18f, s * 0.2f, s * 0.1f, 0.0f,
              RGB(240, 220, 190), 230u);
      for (int32_t i = 0; i < 4; ++i)
        capsule(canvas, cx - s * 0.22f + s * 0.14f * (float)i, cy + s * 0.22f,
                cx - s * 0.24f + s * 0.14f * (float)i, cy + s * 0.38f,
                s * 0.045f, RGB(120, 66, 34), 256u);
      const float hx = cx - s * 0.32f, hy = cy - s * 0.08f;
      triangle(canvas, hx - s * 0.14f, hy - s * 0.08f, hx - s * 0.06f,
               hy - s * 0.3f, hx, hy - s * 0.1f, info->body, 256u);
      triangle(canvas, hx + s * 0.02f, hy - s * 0.1f, hx + s * 0.1f,
               hy - s * 0.3f, hx + s * 0.14f, hy - s * 0.06f, info->body, 256u);
      disc(canvas, hx, hy, s * 0.16f, info->body, 256u);
      ellipse(canvas, hx - s * 0.12f, hy + s * 0.05f, s * 0.08f, s * 0.05f,
              0.0f, RGB(240, 220, 190), 256u);
      disc(canvas, hx - s * 0.19f, hy + s * 0.04f, s * 0.025f, RGB(30, 20, 20),
           256u);
      glow(canvas, hx - s * 0.06f, hy - s * 0.03f, s * 0.08f, info->accent,
           200u);
      disc(canvas, hx - s * 0.06f, hy - s * 0.03f, s * 0.028f, info->accent,
           256u);
      glow(canvas, hx + s * 0.06f, hy - s * 0.03f, s * 0.08f, info->accent,
           200u);
      disc(canvas, hx + s * 0.06f, hy - s * 0.03f, s * 0.028f, info->accent,
           256u);
      break;
    }
    case H2_XIANXIA_RPG_ENEMY_WISP:
      glow(canvas, cx, cy, s * 0.7f, info->body, 120u);
      for (int32_t i = 0; i < 6; ++i) {
        const float k = (float)i / 5.0f;
        const float sway = sinf(phase * 2.0f + k * 3.0f) * s * 0.06f * k;
        disc(canvas, cx + sway, cy + s * (0.2f - 0.5f * k),
             s * (0.3f - 0.2f * k), i < 3 ? info->body : RGB(140, 160, 255),
             230u);
      }
      disc(canvas, cx, cy + s * 0.1f, s * 0.16f, info->accent, 230u);
      ellipse(canvas, cx - s * 0.08f, cy + s * 0.05f, s * 0.04f, s * 0.06f,
              0.0f, RGB(20, 20, 60), 256u);
      ellipse(canvas, cx + s * 0.08f, cy + s * 0.05f, s * 0.04f, s * 0.06f,
              0.0f, RGB(20, 20, 60), 256u);
      for (int32_t i = 0; i < 3; ++i) {
        const float a = phase + (float)i * (2.0f * PI / 3.0f);
        disc(canvas, cx + cosf(a) * s * 0.45f, cy + sinf(a) * s * 0.2f,
             s * 0.04f, info->accent, 220u);
      }
      break;
    case H2_XIANXIA_RPG_ENEMY_FOG_GHOST:
      glow(canvas, cx, cy, s * 0.75f, RGB(230, 236, 244), 130u);
      for (int32_t i = 0; i < 7; ++i) {
        const float k = (float)i / 6.0f;
        const float sway = sinf(phase * 1.5f + k * 4.0f) * s * 0.08f * k;
        disc(canvas, cx + sway, cy + s * (0.3f - 0.62f * k),
             s * (0.34f - 0.22f * k), i < 4 ? info->body : RGB(236, 240, 246),
             200u);
      }
      for (int32_t i = 0; i < 3; ++i)
        capsule(canvas, cx + s * (0.12f * (float)i - 0.12f), cy + s * 0.4f,
                cx + s * (0.16f * (float)i - 0.2f) +
                    sinf(phase + (float)i) * s * 0.06f,
                cy + s * 0.66f, s * 0.05f, info->body, 180u);
      ellipse(canvas, cx - s * 0.09f, cy - s * 0.04f, s * 0.045f, s * 0.07f,
              0.0f, info->accent, 256u);
      ellipse(canvas, cx + s * 0.09f, cy - s * 0.04f, s * 0.045f, s * 0.07f,
              0.0f, info->accent, 256u);
      break;
    case H2_XIANXIA_RPG_ENEMY_FOG_WRAITH: {
      if (charging) {
        const float pulse = 0.5f + 0.5f * sinf(phase * 6.0f);
        for (int32_t i = 0; i < 8; ++i) {
          const float a = (float)i * (2.0f * PI / 8.0f) + phase;
          disc(canvas, cx + cosf(a) * s * 0.62f, cy + sinf(a) * s * 0.3f,
               s * 0.06f, RGB(190, 150, 255),
               (uint32_t)(150.0f + 100.0f * pulse));
        }
      }
      glow(canvas, cx, cy, s * 0.95f, RGB(170, 170, 196), 140u);
      /* A hooded shape of fog around a cracked lamp. */
      for (int32_t i = 0; i < 9; ++i) {
        const float k = (float)i / 8.0f;
        const float sway = sinf(phase + k * 3.0f) * s * 0.07f;
        ellipse(canvas, cx + sway * k, cy + s * (0.5f - 0.9f * k),
                s * (0.46f - 0.22f * k), s * (0.2f - 0.04f * k), 0.0f,
                i % 2 ? info->body : RGB(122, 120, 150), 210u);
      }
      ellipse(canvas, cx, cy - s * 0.4f, s * 0.24f, s * 0.2f, 0.0f,
              RGB(70, 66, 92), 240u);
      glow(canvas, cx, cy + s * 0.05f, s * 0.26f, info->accent, 220u);
      rounded_rect(canvas, cx - s * 0.08f, cy - s * 0.04f, cx + s * 0.08f,
                   cy + s * 0.16f, s * 0.03f, RGB(220, 190, 255), 256u);
      capsule(canvas, cx - s * 0.04f, cy - s * 0.02f, cx + s * 0.03f,
              cy + s * 0.12f, s * 0.012f, RGB(60, 40, 80), 256u);
      disc(canvas, cx - s * 0.08f, cy - s * 0.42f, s * 0.035f,
           RGB(230, 210, 255), 256u);
      disc(canvas, cx + s * 0.08f, cy - s * 0.42f, s * 0.035f,
           RGB(230, 210, 255), 256u);
      for (int32_t i = 0; i < 4; ++i)
        capsule(canvas, cx + s * (0.2f * (float)i - 0.3f), cy + s * 0.5f,
                cx + s * (0.24f * (float)i - 0.4f) +
                    sinf(phase * 1.3f + (float)i) * s * 0.08f,
                cy + s * 0.78f, s * 0.05f, info->body, 160u);
      break;
    }
    case H2_XIANXIA_RPG_ENEMY_GOLEM:
    case H2_XIANXIA_RPG_ENEMY_WRECK_GUARD: {
      if (enemy == H2_XIANXIA_RPG_ENEMY_WRECK_GUARD)
        glow(canvas, cx, cy, s * 0.8f, RGB(190, 210, 226), 120u);
      ellipse(canvas, cx, cy + s * 0.55f, s * 0.46f, s * 0.08f, 0.0f, C_SHADOW,
              120u);
      const uint16_t stone = info->body, dark = RGB(92, 88, 84);
      rounded_rect(canvas, cx - s * 0.12f, cy + s * 0.2f, cx - s * 0.02f,
                   cy + s * 0.55f, s * 0.04f, dark, 256u);
      rounded_rect(canvas, cx + s * 0.04f, cy + s * 0.2f, cx + s * 0.14f,
                   cy + s * 0.55f, s * 0.04f, dark, 256u);
      rounded_rect(canvas, cx - s * 0.3f, cy - s * 0.25f, cx + s * 0.3f,
                   cy + s * 0.28f, s * 0.08f, stone, 256u);
      capsule(canvas, cx - s * 0.34f, cy - s * 0.15f, cx - s * 0.42f,
              cy + s * 0.25f, s * 0.1f, dark, 256u);
      capsule(canvas, cx + s * 0.34f, cy - s * 0.15f, cx + s * 0.42f,
              cy + s * 0.25f, s * 0.1f, dark, 256u);
      rounded_rect(canvas, cx - s * 0.15f, cy - s * 0.48f, cx + s * 0.15f,
                   cy - s * 0.24f, s * 0.05f, stone, 256u);
      ellipse(canvas, cx - s * 0.12f, cy - s * 0.2f, s * 0.12f, s * 0.05f, 0.4f,
              RGB(80, 130, 70), 200u);
      glow(canvas, cx, cy, s * 0.22f, info->accent, 150u);
      capsule(canvas, cx - s * 0.1f, cy - s * 0.05f, cx + s * 0.1f,
              cy + s * 0.08f, s * 0.02f, info->accent, 256u);
      capsule(canvas, cx + s * 0.1f, cy - s * 0.05f, cx - s * 0.06f,
              cy + s * 0.1f, s * 0.02f, info->accent, 256u);
      disc(canvas, cx - s * 0.06f, cy - s * 0.37f, s * 0.03f, info->accent,
           256u);
      disc(canvas, cx + s * 0.06f, cy - s * 0.37f, s * 0.03f, info->accent,
           256u);
      break;
    }
    case H2_XIANXIA_RPG_ENEMY_INK_TIDE:
    case H2_XIANXIA_RPG_ENEMY_DRAGON: {
      if (charging) {
        const float pulse = 0.5f + 0.5f * sinf(phase * 6.0f);
        glow(canvas, cx, cy, s * (0.9f + 0.15f * pulse), RGB(110, 60, 170),
             (uint32_t)(150.0f + 80.0f * pulse));
      }
      glow(canvas, cx, cy + s * 0.2f, s * 0.9f, RGB(30, 30, 50), 140u);
      for (int32_t i = 0; i < 22; ++i) {
        const float k = (float)i / 21.0f;
        const float x = cx + s * (0.7f - 1.15f * k);
        const float y =
            cy + s * (0.35f - 0.55f * k) + sinf(k * 7.0f + phase) * s * 0.12f;
        const float r = s * (0.08f + 0.12f * sinf(k * PI * 0.9f + 0.2f));
        disc(canvas, x, y - 1.5f, r + 3.0f, RGB(126, 110, 178), 230u);
      }
      /* Body: a chain of segments along a sine from tail to head. */
      for (int32_t i = 0; i < 22; ++i) {
        const float k = (float)i / 21.0f;
        const float x = cx + s * (0.7f - 1.15f * k);
        const float y =
            cy + s * (0.35f - 0.55f * k) + sinf(k * 7.0f + phase) * s * 0.12f;
        const float r = s * (0.08f + 0.12f * sinf(k * PI * 0.9f + 0.2f));
        disc(canvas, x, y, r, info->body, 256u);
        disc(canvas, x - r * 0.2f, y + r * 0.4f, r * 0.55f, RGB(150, 140, 120),
             150u);
        if (i % 3 == 0 && i > 2)
          triangle(canvas, x - r * 0.5f, y - r * 0.6f, x + r * 0.5f,
                   y - r * 0.6f, x + r * 0.1f, y - r * 1.6f, RGB(70, 40, 90),
                   230u);
      }
      const float hx = cx - s * 0.48f;
      const float hy = cy - s * 0.25f + sinf(phase) * s * 0.03f;
      ellipse(canvas, hx, hy - 1.5f, s * 0.26f + 3.0f, s * 0.16f + 3.0f, -0.15f,
              RGB(126, 110, 178), 230u);
      ellipse(canvas, hx, hy, s * 0.26f, s * 0.16f, -0.15f, RGB(52, 56, 80),
              256u);
      ellipse(canvas, hx - s * 0.2f, hy + s * 0.04f, s * 0.14f, s * 0.08f,
              -0.1f, RGB(60, 64, 90), 256u);
      capsule(canvas, hx + s * 0.08f, hy - s * 0.1f, hx + s * 0.3f,
              hy - s * 0.42f, s * 0.035f, RGB(226, 206, 150), 256u);
      capsule(canvas, hx - s * 0.04f, hy - s * 0.12f, hx + s * 0.08f,
              hy - s * 0.44f, s * 0.035f, RGB(226, 206, 150), 256u);
      glow(canvas, hx - s * 0.08f, hy - s * 0.04f, s * 0.12f, info->accent,
           220u);
      disc(canvas, hx - s * 0.08f, hy - s * 0.04f, s * 0.04f, info->accent,
           256u);
      capsule(canvas, hx - s * 0.32f, hy + s * 0.06f, hx - s * 0.6f,
              hy + s * 0.26f + sinf(phase * 2.0f) * s * 0.05f, s * 0.012f,
              RGB(210, 210, 220), 220u);
      capsule(canvas, hx - s * 0.3f, hy + s * 0.1f, hx - s * 0.5f,
              hy + s * 0.4f + sinf(phase * 2.0f + 1.0f) * s * 0.05f, s * 0.012f,
              RGB(210, 210, 220), 220u);
      break;
    }
    default:
      break;
  }
}

/* ---- Keys -------------------------------------------------------------- */

void h2_xianxia_rpg_background_key(const game_t* game,
                                   h2_xianxia_rpg_layer_key_t* out) {
  memset(out, 0, sizeof(*out));
  out->fields[0] = (int32_t)game->phase;
  if (game->phase == H2_XIANXIA_RPG_PHASE_EXPLORE ||
      game->phase == H2_XIANXIA_RPG_PHASE_BATTLE) {
    out->fields[1] = (int32_t)game->map;
    out->fields[2] = (int32_t)game->stage;
    /* Lamps, fog walls and the guard change chapter-2 maps. */
    out->fields[3] = (int32_t)game->flags;
    out->fields[4] = (int32_t)game->facts;
  } else if (game->phase == H2_XIANXIA_RPG_PHASE_ENDING) {
    out->fields[1] = (int32_t)game->chapter;
  } else if (game->phase == H2_XIANXIA_RPG_PHASE_PROLOGUE) {
    out->fields[1] = (int32_t)game->prologue_chapter;
  }
}

/* What the player should do now in a companion battle, as a ribbon text
 * id, or TEXT_COUNT for none. */
static h2_xianxia_rpg_text_id_t battle_hint(const game_t* game) {
  const h2_xianxia_rpg_battle_t* b = &game->battle;
  if (b->enemy == H2_XIANXIA_RPG_ENEMY_MIRROR) {
    if (b->mirror_command == H2_XIANXIA_RPG_COMMAND_SKILL && b->mirror_joint)
      return H2_XIANXIA_RPG_TEXT_C7_M_JOINT;
    return (h2_xianxia_rpg_text_id_t)(H2_XIANXIA_RPG_TEXT_C7_M_ATTACK +
                                      b->mirror_command);
  }
  if (b->lamp_battle) {
    if (b->state == H2_XIANXIA_RPG_BATTLE_HEAL_TARGET)
      return H2_XIANXIA_RPG_TEXT_C4_HEAL;
    if (b->state != H2_XIANXIA_RPG_BATTLE_COMMAND)
      return H2_XIANXIA_RPG_TEXT_COUNT;
    return b->wave_turn == 2u ? H2_XIANXIA_RPG_TEXT_C4_WAVE_HINT
           : h2_xianxia_rpg_lamp_target(game) == 2u
               ? H2_XIANXIA_RPG_TEXT_C4_LAMP_HINT
               : H2_XIANXIA_RPG_TEXT_C4_ALLY_HINT;
  }
  if (!b->ally) return H2_XIANXIA_RPG_TEXT_COUNT;
  if (b->state == H2_XIANXIA_RPG_BATTLE_TARGET)
    return H2_XIANXIA_RPG_TEXT_HINT_TARGET;
  if (b->state != H2_XIANXIA_RPG_BATTLE_COMMAND)
    return H2_XIANXIA_RPG_TEXT_COUNT;
  if (b->enemy == H2_XIANXIA_RPG_ENEMY_SWORD_SOUL ||
      b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU) {
    const uint32_t learned = H2_XIANXIA_RPG_FLAG_C3_STELE1 << b->sword_stance;
    if (!(game->flags & learned))
      return b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU
                 ? (h2_xianxia_rpg_text_id_t)(H2_XIANXIA_RPG_TEXT_C5_RAIN +
                                              b->sword_stance)
                 : H2_XIANXIA_RPG_TEXT_C3_UNKNOWN;
    return (h2_xianxia_rpg_text_id_t)(H2_XIANXIA_RPG_TEXT_C3_RAIN_HINT +
                                      b->sword_stance);
  }
  const bool joint = h2_xianxia_rpg_joint_ready(game);
  if (b->tutorial) {
    if (joint) return H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL3;
    return b->veiled ? H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL1
                     : H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL2;
  }
  if (b->mp_sealed) return H2_XIANXIA_RPG_TEXT_HINT_SEALED;
  if (b->charging) return H2_XIANXIA_RPG_TEXT_HINT_CHARGE;
  if (b->split) return H2_XIANXIA_RPG_TEXT_HINT_SPLIT;
  if (joint) return H2_XIANXIA_RPG_TEXT_HINT_JOINT;
  if (b->veiled) return H2_XIANXIA_RPG_TEXT_HINT_VEILED;
  return H2_XIANXIA_RPG_TEXT_COUNT;
}

void h2_xianxia_rpg_static_key(const game_t* game,
                               h2_xianxia_rpg_layer_key_t* out) {
  memset(out, 0, sizeof(*out));
  int32_t* f = out->fields;
  f[0] = (int32_t)game->phase;
  switch (game->phase) {
    case H2_XIANXIA_RPG_PHASE_TITLE: {
      const h2_xianxia_rpg_save_t* resume = h2_xianxia_rpg_continue_save(game);
      f[1] = resume != NULL ? (int32_t)resume->chapter : 0;
      f[2] = game->records.unreadable ? 1 : 0;
      break;
    }
    case H2_XIANXIA_RPG_PHASE_CHAPTERS:
      f[1] = (int32_t)game->records.progress.cleared;
      for (uint32_t c = 1u; c <= H2_XIANXIA_RPG_CHAPTERS_PLAYABLE; ++c)
        f[1 + c] = h2_xianxia_rpg_unfinished(game, c) != NULL ? 1 : 0;
      break;
    case H2_XIANXIA_RPG_PHASE_PROLOGUE:
      f[1] = game->time_ms >= game->phase_started_ms + 900u ? 1 : 0;
      f[2] = (int32_t)game->prologue_chapter;
      f[3] = game->prologue_carry ? 1 : 0;
      break;
    case H2_XIANXIA_RPG_PHASE_EXPLORE:
    case H2_XIANXIA_RPG_PHASE_BATTLE:
      f[1] = (int32_t)game->map;
      f[2] = (int32_t)game->stage;
      f[3] = (int32_t)game->hero.hp;
      f[4] = (int32_t)game->hero.mp;
      f[5] = (int32_t)game->hero.level;
      f[6] = (int32_t)game->hero.pills;
      if (game->phase == H2_XIANXIA_RPG_PHASE_EXPLORE) {
        f[7] = game->dialogue.active ? 1 : 0;
        f[8] = game->dialogue.active ? (int32_t)game->dialogue.index : 0;
        f[9] = game->dialogue.active ? (int32_t)game->dialogue.revealed : 0;
        /* Scripts belong to separate chapter arrays; pointer subtraction
         * between those arrays is undefined. Fold an integer identity. */
        const uintptr_t script =
            game->dialogue.active ? (uintptr_t)game->dialogue.script : 0u;
        f[10] = (int32_t)(script ^ (script >> 16) ^ game->dialogue.page_offset);
        f[11] = game->banner_ms > 0u ? 1 : 0;
        f[12] = game->objective_ms > 0u ? 1 : 0;
      } else {
        const h2_xianxia_rpg_battle_t* b = &game->battle;
        f[7] = (int32_t)b->state;
        f[8] = (int32_t)b->message_id;
        f[9] = (int32_t)b->enemy_hp;
        f[10] = (int32_t)b->enemy;
        f[11] = (b->state == H2_XIANXIA_RPG_BATTLE_VICTORY ||
                 b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT) &&
                        b->state_ms >= 450u
                    ? 1
                    : 0;
        int32_t available = 0;
        for (int32_t i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i)
          if (h2_xianxia_rpg_command_available(game,
                                               (h2_xianxia_rpg_command_t)i))
            available |= 1 << i;
        f[12] = available;
        f[13] = (int32_t)b->rapport;
        f[14] = (b->veiled ? 1 : 0) | (b->mp_sealed ? 2 : 0) |
                (b->split ? 4 : 0) | (b->charging ? 8 : 0) |
                (h2_xianxia_rpg_joint_ready(game) ? 16 : 0);
        f[15] = (int32_t)b->real_shadow;
        f[16] = (int32_t)battle_hint(game);
      }
      f[17] = (int32_t)game->chapter;
      f[18] = (int32_t)game->flags;
      f[19] = (int32_t)game->ally_hp;
      f[20] = (int32_t)game->battle.lamp_hp;
      f[21] = (int32_t)game->battle.wave;
      f[22] = (int32_t)(game->chapter == 5u ? game->battle.turn
                                            : game->battle.wave_turn);
      f[23] = game->chapter == 7u ? (int32_t)(game->facts | (game->trust << 16))
              : game->chapter == 6u
                  ? (int32_t)(game->facts | ((game->turns_taken % 6u) << 8))
              : game->battle.ally_mend ? 1
                                       : 0;
      break;
    case H2_XIANXIA_RPG_PHASE_ENDING:
      f[1] = game->time_ms >= game->phase_started_ms + 1500u ? 1 : 0;
      f[2] = (int32_t)game->hero.level;
      f[3] = (int32_t)game->chapter;
      break;
  }
}

/* ---- Static layers ----------------------------------------------------- */

/* A gold-rimmed button; disabled ones are grey. */
static void button(canvas_t* canvas, const h2_xianxia_rpg_rect_t* r,
                   const char* label, bool enabled) {
  const float x0 = (float)r->x, y0 = (float)r->y;
  const float x1 = x0 + (float)r->w, y1 = y0 + (float)r->h;
  rounded_rect(canvas, x0 + 3.0f, y0 + 4.0f, x1 + 3.0f, y1 + 4.0f, 10.0f,
               RGB(60, 50, 40), 70u);
  rounded_rect(canvas, x0, y0, x1, y1, 10.0f,
               enabled ? C_GOLD : RGB(170, 160, 150), 256u);
  rounded_rect(canvas, x0 + 2.0f, y0 + 2.0f, x1 - 2.0f, y1 - 2.0f, 8.5f,
               enabled ? RGB(48, 40, 46) : RGB(120, 114, 110), 256u);
  text_centered(canvas, false, label, (x0 + x1) * 0.5f,
                (int32_t)((y0 + y1) * 0.5f) - LINE_PX / 2,
                enabled ? C_TEXT : RGB(200, 194, 186), 256u);
}

static void draw_title(canvas_t* canvas, const game_t* game) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const lv_font_t* big = font_of(true);
  const char* title = h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_TITLE];
  const int32_t title_w = h2_xianxia_rpg_text_width(true, title);
  const int32_t ty = (int32_t)(h * 0.2f);
  /* Vertical calligraphy feel: wide letter spacing via per-glyph draw. */
  const int32_t spacing = big->line_height / 3;
  const int32_t glyphs = (int32_t)h2_xianxia_rpg_utf8_length(title);
  const int32_t total = title_w + spacing * (glyphs - 1);
  int32_t pen = (int32_t)(w * 0.5f) - total / 2;
  for (const char* s = title; *s != '\0';) {
    const uint32_t cp = h2_xianxia_rpg_utf8_next(&s);
    (void)glyph(canvas, big, cp, pen + 2, ty + 3, RGB(160, 140, 120), 160u);
    pen += glyph(canvas, big, cp, pen, ty, C_INK, 256u) + spacing;
  }
  /* Red seal beside the title. */
  const float seal = (float)big->line_height * 0.62f;
  const float sx = w * 0.5f + (float)total * 0.5f + seal * 0.4f;
  const float sy = (float)ty + (float)big->line_height * 0.25f;
  rounded_rect(canvas, sx, sy, sx + seal, sy + seal, seal * 0.12f,
               RGB(190, 40, 36), 240u);
  (void)glyph(canvas, font_of(false), 0x4ED9u, (int32_t)(sx + seal * 0.5f) - 9,
              (int32_t)(sy + seal * 0.5f) - LINE_PX / 2, RGB(250, 236, 220),
              256u);
  /* Small screens keep the chapter names and drop the subtitle. */
  const int32_t first_button = game->layout.title_buttons[0].y;
  const int32_t sub_y = ty + big->line_height + 10;
  const bool roomy = sub_y + 2 * LINE_PX + 10 <= first_button;
  if (roomy)
    paper_centered(canvas, false,
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_SUBTITLE], w * 0.5f,
                   sub_y, RGB(80, 70, 70), 240u);
  const int32_t chapter_y = roomy ? sub_y + LINE_PX + 6 : sub_y;
  if (chapter_y + LINE_PX <= first_button)
    paper_centered(canvas, false,
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_CHAPTER], w * 0.5f,
                   chapter_y, RGB(170, 44, 38), 256u);
  const h2_xianxia_rpg_save_t* resume = h2_xianxia_rpg_continue_save(game);
  char resume_label[64];
  const h2_xianxia_rpg_chapter_info_t* resume_info =
      resume != NULL ? h2_xianxia_rpg_chapter_info(resume->chapter) : NULL;
  if (resume_info != NULL)
    (void)snprintf(resume_label, sizeof(resume_label),
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_CONTINUE_AT],
                   resume_info->label);
  else
    (void)snprintf(resume_label, sizeof(resume_label), "%s",
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_CONTINUE]);
  button(canvas, &game->layout.title_buttons[0],
         h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_NEW_GAME], true);
  button(canvas, &game->layout.title_buttons[1], resume_label, resume != NULL);
  button(canvas, &game->layout.title_buttons[2],
         h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_CHAPTERS], true);
  const h2_xianxia_rpg_rect_t* last =
      &game->layout.title_buttons[H2_XIANXIA_RPG_TITLE_BUTTONS - 1];
  const int32_t hint_y = (int32_t)(h - (float)LINE_PX - 10.0f);
  if (hint_y >= last->y + last->h + 4)
    paper_centered(
        canvas, false,
        h2_xianxia_rpg_text[game->records.unreadable
                                ? H2_XIANXIA_RPG_TEXT_UNREADABLE
                                : H2_XIANXIA_RPG_TEXT_TITLE_HINT],
        w * 0.5f, hint_y,
        game->records.unreadable ? RGB(170, 44, 38) : RGB(110, 100, 96), 220u);
}

/* Chapter rows: unfinished chapters resume, others start afresh. */
static void draw_chapters(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const h2_xianxia_rpg_rect_t* p = &l->chapter_panel;
  panel(canvas, (float)p->x, (float)p->y, (float)p->w, (float)p->h, 12.0f,
        236u);
  text_centered(canvas, false,
                h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_CHAPTERS_HEAD],
                (float)p->x + (float)p->w * 0.5f, p->y + 12, C_GOLD, 256u);
  for (int32_t i = 0; i < H2_XIANXIA_RPG_CHAPTER_ROWS; ++i) {
    const h2_xianxia_rpg_rect_t* r = &l->chapter_rows[i];
    const uint32_t chapter = (uint32_t)i + 1u;
    const h2_xianxia_rpg_chapter_info_t* info =
        chapter <= H2_XIANXIA_RPG_CHAPTERS_PLAYABLE
            ? h2_xianxia_rpg_chapter_info(chapter)
            : NULL;
    const bool enabled = info != NULL;
    const bool unfinished =
        enabled && h2_xianxia_rpg_unfinished(game, chapter) != NULL;
    const bool cleared =
        enabled && (game->records.progress.cleared >> i & 1u) != 0u;
    const float x0 = (float)r->x, y0 = (float)r->y;
    const float x1 = x0 + (float)r->w, y1 = y0 + (float)r->h;
    rounded_rect(canvas, x0, y0, x1, y1, 8.0f,
                 enabled ? C_GOLD : RGB(96, 90, 86), 230u);
    rounded_rect(canvas, x0 + 2.0f, y0 + 2.0f, x1 - 2.0f, y1 - 2.0f, 6.5f,
                 enabled ? RGB(56, 40, 40) : RGB(40, 36, 40), 256u);
    const char* name = enabled
                           ? info->name
                           : h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_UPCOMING];
    const int32_t text_y = r->y + (r->h - LINE_PX) / 2;
    const int32_t name_end =
        r->x + 12 +
        text_at(canvas, false, name, r->x + 12, text_y,
                enabled ? C_TEXT : RGB(130, 124, 120), 256u);
    int32_t right = r->x + r->w - 12;
    if (unfinished) {
      button(canvas, &l->chapter_restart[i],
             h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_RESTART], true);
      right = l->chapter_restart[i].x - 8;
    }
    const char* status =
        h2_xianxia_rpg_text[!enabled     ? H2_XIANXIA_RPG_TEXT_STATUS_SOON
                            : unfinished ? H2_XIANXIA_RPG_TEXT_STATUS_PLAYING
                            : cleared    ? H2_XIANXIA_RPG_TEXT_STATUS_CLEARED
                                         : H2_XIANXIA_RPG_TEXT_STATUS_OPEN];
    const int32_t status_w = h2_xianxia_rpg_text_width(false, status);
    if (right - status_w >= name_end + 8)
      (void)text_at(canvas, false, status, right - status_w, text_y,
                    cleared && !unfinished ? RGB(150, 220, 140)
                    : enabled              ? C_GOLD
                                           : RGB(130, 124, 120),
                    240u);
  }
  button(canvas, &l->chapter_back,
         h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_BACK], true);
}

/* A parchment card centered on the screen; returns its top. */
static float card(canvas_t* canvas, float card_h) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const float pw = fminf(w * 0.9f, 560.0f);
  const float ch = fminf(card_h, h - 12.0f);
  const float px = (w - pw) * 0.5f, py = h * 0.5f - ch * 0.5f;
  rounded_rect(canvas, px, py, px + pw, py + ch, 16.0f, RGB(250, 244, 230),
               210u);
  return py;
}

/* The chapter card: name, story so far and the hero carried into it. */
static void draw_prologue(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_chapter_info_t* info =
      h2_xianxia_rpg_chapter_info(game->prologue_chapter);
  if (info == NULL) return;
  const float w = (float)canvas->width, h = (float)canvas->height;
  const int32_t big = font_of(true)->line_height;
  const float pw = fminf(w * 0.9f, 560.0f);
  const int32_t text_w = (int32_t)pw - 40;
  const int32_t text_x = (int32_t)((w - pw) * 0.5f) + 20;
  const bool roomy = h >= 400.0f;
  const int32_t recap_lines = roomy ? 5 : 3;
  const float ch = (float)(big + (roomy ? 2 : 1) * LINE_PX +
                           (recap_lines + 3) * LINE_PX + 48);
  int32_t y = (int32_t)card(canvas, ch) + 12;
  if (roomy) {
    paper_centered(canvas, false, info->label, w * 0.5f, y, RGB(120, 90, 70),
                   240u);
    y += LINE_PX;
  }
  paper_centered(canvas, true, info->title, w * 0.5f, y, RGB(170, 40, 36),
                 256u);
  y += big + 4;
  if (info->recap != NULL) {
    paper_centered(canvas, false,
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_RECAP_HEAD],
                   w * 0.5f, y, RGB(150, 60, 40), 256u);
    y += LINE_PX + 2;
    y += LINE_PX * wrapped_with(canvas, info->recap, text_x, y, text_w,
                                recap_lines, C_INK, 256u, UINT32_MAX,
                                RGB(255, 252, 240));
    y += 6;
  }
  char carry[128];
  const h2_xianxia_rpg_clear_t* from =
      game->prologue_carry && game->prologue_chapter >= 2u
          ? &game->records.progress.chapters[game->prologue_chapter - 2u]
          : NULL;
  if (from != NULL)
    (void)snprintf(carry, sizeof(carry),
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_CARRY],
                   (unsigned)from->level, (unsigned)from->pills);
  else
    (void)snprintf(
        carry, sizeof(carry), h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_DEFAULTS],
        (unsigned)info->default_level, (unsigned)info->default_pills);
  y +=
      LINE_PX * wrapped_with(canvas, carry, text_x, y, text_w, 2,
                             from != NULL ? RGB(40, 110, 70) : RGB(150, 90, 40),
                             256u, UINT32_MAX, RGB(255, 252, 240));
  y += 8;
  if (game->time_ms >= game->phase_started_ms + 900u)
    paper_centered(canvas, false,
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_PROLOGUE_HINT],
                   w * 0.5f, y, RGB(150, 110, 60), 240u);
}

static void draw_ending(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_chapter_info_t* info =
      h2_xianxia_rpg_chapter_info(game->chapter);
  if (info == NULL) info = h2_xianxia_rpg_chapter_info(1u);
  const float w = (float)canvas->width;
  const int32_t big = font_of(true)->line_height;
  int32_t y = (int32_t)card(canvas, (float)(big + 6 * LINE_PX + 48)) + 14;
  paper_centered(canvas, true, info->title, w * 0.5f, y, RGB(170, 40, 36),
                 256u);
  y += big + 8;
  paper_centered(canvas, false, info->ending[0], w * 0.5f, y, C_INK, 256u);
  y += LINE_PX + 4;
  paper_centered(canvas, false, info->ending[1], w * 0.5f, y, C_INK, 256u);
  y += LINE_PX + 6;
  paper_centered(canvas, false, info->next, w * 0.5f, y, RGB(170, 44, 38),
                 256u);
  y += LINE_PX + 12;
  char stats[96];
  (void)snprintf(stats, sizeof(stats),
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_ENDING_STATS],
                 (unsigned)game->hero.level, (unsigned)game->battles_won,
                 (unsigned)game->turns_taken);
  paper_centered(canvas, false, stats, w * 0.5f, y, RGB(90, 80, 80), 240u);
  y += LINE_PX + 6;
  /* Chapter 1 leads straight into chapter 2. */
  const bool onward = game->chapter < H2_XIANXIA_RPG_CHAPTERS_PLAYABLE;
  if (game->time_ms >= game->phase_started_ms + 1500u)
    paper_centered(
        canvas, false,
        h2_xianxia_rpg_text
            [onward
                 ? (game->chapter == 6u   ? H2_XIANXIA_RPG_TEXT_C7_ONWARD
                    : game->chapter == 5u ? H2_XIANXIA_RPG_TEXT_C6_ONWARD
                    : game->chapter == 4u ? H2_XIANXIA_RPG_TEXT_C5_ONWARD
                    : game->chapter == 3u ? H2_XIANXIA_RPG_TEXT_C4_ONWARD
                    : game->chapter == 2u ? H2_XIANXIA_RPG_TEXT_C3_ONWARD
                                          : H2_XIANXIA_RPG_TEXT_ENDING_ONWARD)
                 : H2_XIANXIA_RPG_TEXT_ENDING_HINT],
        w * 0.5f, y, RGB(150, 110, 60), 240u);
}

static void hero_status(canvas_t* canvas, const game_t* game, int32_t x,
                        int32_t y, int32_t w, bool stacked) {
  const h2_xianxia_rpg_hero_t* hero = &game->hero;
  char value[32];
  int32_t pen = x;
  pen += text_at(canvas, false,
                 h2_xianxia_rpg_actor_name(H2_XIANXIA_RPG_ACTOR_HERO), pen, y,
                 C_GOLD, 256u) +
         6;
  (void)snprintf(value, sizeof(value),
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_LEVEL],
                 (unsigned)hero->level);
  pen += text_at(canvas, false, value, pen, y, C_TEXT, 240u) + 12;
  (void)snprintf(value, sizeof(value),
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_PILLS],
                 (unsigned)hero->pills);
  const int32_t pills_w = h2_xianxia_rpg_text_width(false, value);
  if (stacked) {
    (void)text_at(canvas, false, value, x + w - pills_w, y, C_TEXT, 240u);
    const int32_t half = (w - 12) / 2;
    labeled_bar(canvas, h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_HP], x,
                y + LINE_PX + 2, half, hero->hp, hero->hp_max, C_HP);
    labeled_bar(canvas, h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_MP],
                x + half + 12, y + LINE_PX + 2, half, hero->mp, hero->mp_max,
                C_MP);
    return;
  }
  const int32_t bars = x + w - pills_w - 12 - pen;
  const int32_t hp_w = bars * 55 / 100;
  labeled_bar(canvas, h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_HP], pen, y,
              hp_w - 8, hero->hp, hero->hp_max, C_HP);
  labeled_bar(canvas, h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_MP], pen + hp_w,
              y, bars - hp_w, hero->mp, hero->mp_max, C_MP);
  (void)text_at(canvas, false, value, x + w - pills_w, y, C_TEXT, 240u);
}

static void draw_hud(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const h2_xianxia_rpg_rect_t* r = &l->hud;
  fill_rect(canvas, (float)r->x, (float)r->y, (float)r->w, (float)r->h,
            RGB(22, 18, 26), 236u);
  fill_rect(canvas, (float)r->x, (float)(r->y + r->h - 2), (float)r->w, 2.0f,
            C_GOLD, 200u);
  const int32_t x = r->x + 10, w = r->w - 20;
  int32_t y = r->y + 5;
  if (game->chapter == 7u) {
    text_wrapped(canvas, h2_xianxia_rpg_objective(game->stage), x, y, w, 1,
                 C_TEXT, 256u, UINT32_MAX);
    char status[128];
    (void)snprintf(status, sizeof(status),
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C7_STATUS],
                   h2_xianxia_rpg_text[game->flags & H2_XIANXIA_RPG_FLAG_C7_PAST
                                           ? H2_XIANXIA_RPG_TEXT_C7_PAST
                                           : H2_XIANXIA_RPG_TEXT_C7_NOW],
                   (unsigned)game->trust);
    text_wrapped(
        canvas, status, x, y + LINE_PX + 3, w, 1,
        game->flags & H2_XIANXIA_RPG_FLAG_C7_PAST ? C_GOLD : RGB(164, 227, 245),
        256u, UINT32_MAX);
    return;
  }
  if (game->chapter == 6u) {
    text_wrapped(canvas, h2_xianxia_rpg_objective(game->stage), x, y, w, 1,
                 C_TEXT, 256u, UINT32_MAX);
    const uint32_t meter = game->flags & H2_XIANXIA_RPG_C6_METER_MASK;
    char status[128];
    (void)snprintf(
        status, sizeof(status),
        h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C6_STATUS],
        h2_xianxia_rpg_text[meter < 3u   ? H2_XIANXIA_RPG_TEXT_C6_CHILL
                            : meter > 5u ? H2_XIANXIA_RPG_TEXT_C6_WARM
                                         : H2_XIANXIA_RPG_TEXT_C6_STEADY],
        (unsigned)meter,
        h2_xianxia_rpg_text[game->map == H2_XIANXIA_RPG_MAP_HOT_SPRING
                                ? H2_XIANXIA_RPG_TEXT_C6_SPRING
                            : game->map == H2_XIANXIA_RPG_MAP_FROZEN_CREEK
                                ? H2_XIANXIA_RPG_TEXT_C6_CREEK
                                : H2_XIANXIA_RPG_TEXT_C6_HUT]);
    text_wrapped(canvas, status, x, y + LINE_PX + 3, w, 1,
                 meter < 3u   ? RGB(164, 227, 245)
                 : meter > 5u ? RGB(255, 181, 125)
                              : C_GOLD,
                 256u, UINT32_MAX);
    return;
  }
  if (game->chapter == 5u) {
    const char* goal = game->map == H2_XIANXIA_RPG_MAP_WEN_TOWER &&
                               game->stage == H2_XIANXIA_RPG_STAGE_C5_TOWER
                           ? h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C5_VISIT]
                           : h2_xianxia_rpg_objective(game->stage);
    text_wrapped(canvas, goal, x, y, w, 1, C_TEXT, 256u, UINT32_MAX);
    char status[96];
    if (game->stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT &&
        !(game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH)) {
      (void)snprintf(status, sizeof(status),
                     h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C5_STEPS],
                     (unsigned)((game->flags & H2_XIANXIA_RPG_C5_STEPS_MASK) >>
                                H2_XIANXIA_RPG_C5_STEPS_SHIFT));
    } else {
      (void)snprintf(
          status, sizeof(status), "%s",
          h2_xianxia_rpg_text[game->flags & H2_XIANXIA_RPG_FLAG_C5_HIGH
                                  ? H2_XIANXIA_RPG_TEXT_C5_HIGH
                                  : H2_XIANXIA_RPG_TEXT_C5_LOW]);
    }
    text_wrapped(canvas, status, x, y + LINE_PX + 3, w, 1, C_GOLD, 256u,
                 UINT32_MAX);
    return;
  }
  if (game->chapter == 4u) {
    (void)text_at(canvas, false, h2_xianxia_rpg_objective(game->stage), x, y,
                  C_TEXT, 256u);
    char status[80];
    (void)snprintf(status, sizeof(status),
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C4_ESCORT],
                   h2_xianxia_rpg_text[game->flags & H2_XIANXIA_RPG_FLAG_C4_FIRE
                                           ? H2_XIANXIA_RPG_TEXT_C4_FIRE_LIT
                                           : H2_XIANXIA_RPG_TEXT_C4_FIRE_NONE],
                   (unsigned)h2_xianxia_rpg_c4_lamps(game->flags));
    (void)text_at(canvas, false, status, x, y + LINE_PX + 3, C_GOLD, 256u);
    return;
  }
  hero_status(canvas, game, x, y, w, l->stacked);
  y += l->stacked ? 2 * LINE_PX + 4 : LINE_PX + 3;
  char line[160];
  (void)snprintf(line, sizeof(line),
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_OBJECTIVE],
                 h2_xianxia_rpg_objective(game->stage));
  const char* place = h2_xianxia_rpg_map_name(game->map);
  const int32_t place_w = h2_xianxia_rpg_text_width(false, place);
  const int32_t line_w = h2_xianxia_rpg_text_width(false, line);
  int32_t pen = x;
  if (place_w + 16 + line_w <= w) {
    diamond(canvas, (float)pen + 5.0f, (float)y + (float)LINE_PX * 0.5f, 5.0f,
            RGB(190, 40, 36), 256u);
    pen += 14;
    pen += text_at(canvas, false, place, pen, y, C_GOLD, 256u) + 12;
  }
  (void)text_at(canvas, false, line, pen, y, C_TEXT, 240u);
}

static void draw_dialogue(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const h2_xianxia_rpg_rect_t* r = &l->dialog;
  const h2_xianxia_rpg_dialogue_t* d = &game->dialogue;
  if (!d->active && !l->stacked) return;
  panel(canvas, (float)r->x, (float)r->y, (float)r->w, (float)r->h, 10.0f,
        d->active ? 256u : 200u);
  const int32_t x = r->x + 14, y = r->y + 8, w = r->w - 28;
  if (!d->active) {
    text_wrapped(canvas, h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_TIPS], x,
                 y + LINE_PX / 2, w, l->dialog_lines, C_DIM, 220u, UINT32_MAX);
    return;
  }
  const h2_xianxia_rpg_line_t* line = &d->script->lines[d->index];
  const bool narrator = line->speaker == H2_XIANXIA_RPG_ACTOR_NARRATOR;
  int32_t text_y = y;
  if (!narrator) {
    const char* name =
        h2_xianxia_rpg_actor_name((h2_xianxia_rpg_actor_t)line->speaker);
    const int32_t nw = h2_xianxia_rpg_text_width(false, name);
    rounded_rect(canvas, (float)x - 6.0f, (float)y, (float)(x + nw + 6),
                 (float)(y + LINE_PX), 6.0f, RGB(70, 40, 36), 230u);
    (void)text_at(canvas, false, name, x, y, C_GOLD, 256u);
    text_y += LINE_PX + 2;
  }
  text_wrapped(canvas, line->text + d->page_offset, x, text_y, w,
               narrator ? l->dialog_lines + 1 : l->dialog_lines,
               narrator ? C_NARRATION : C_TEXT, 256u, d->revealed);
  if (d->revealed >= d->total) {
    const float ax = (float)(r->x + r->w - 18), ay = (float)(r->y + r->h - 16);
    triangle(canvas, ax - 6.0f, ay - 4.0f, ax + 6.0f, ay - 4.0f, ax, ay + 4.0f,
             C_GOLD, 256u);
  }
}

static void draw_objective_banner(canvas_t* canvas, const game_t* game) {
  if (game->objective_ms == 0u) return;
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const char* head = h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_OBJECTIVE_UPDATE];
  const char* goal = h2_xianxia_rpg_objective(game->stage);
  const float hw = (float)h2_xianxia_rpg_text_width(false, head);
  const float gw = (float)h2_xianxia_rpg_text_width(false, goal);
  const float cx = (float)l->map_x + (float)(COLS * l->tile) * 0.5f;
  const float y = (float)l->map_y + (float)l->tile * 0.5f;
  const float content = hw + 20.0f + gw;
  const float half = content * 0.5f + 18.0f;
  rounded_rect(canvas, cx - half, y, cx + half, y + (float)LINE_PX + 12.0f,
               8.0f, C_GOLD, 230u);
  rounded_rect(canvas, cx - half + 2.0f, y + 2.0f, cx + half - 2.0f,
               y + (float)LINE_PX + 10.0f, 6.5f, RGB(74, 30, 26), 240u);
  const int32_t pen = (int32_t)(cx - content * 0.5f);
  (void)text_at(canvas, false, head, pen, (int32_t)y + 6, C_GOLD, 256u);
  diamond(canvas, (float)pen + hw + 10.0f, y + 6.0f + (float)LINE_PX * 0.5f,
          5.0f, C_GOLD, 256u);
  (void)text_at(canvas, false, goal, pen + (int32_t)(hw + 20.0f),
                (int32_t)y + 6, C_TEXT, 256u);
}

static void draw_banner(canvas_t* canvas, const game_t* game) {
  if (game->dialogue.active) return;
  if (game->banner_ms == 0u) {
    draw_objective_banner(canvas, game);
    return;
  }
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const char* name = h2_xianxia_rpg_map_name(game->map);
  const float tw = (float)h2_xianxia_rpg_text_width(false, name);
  const float cx = (float)l->map_x + (float)(COLS * l->tile) * 0.5f;
  const float y = (float)l->map_y + (float)l->tile * 0.5f;
  const float half = tw * 0.5f + 40.0f;
  rounded_rect(canvas, cx - half, y, cx + half, y + (float)LINE_PX + 12.0f,
               8.0f, RGB(20, 16, 24), 200u);
  capsule(canvas, cx - half + 10.0f, y + 18.0f, cx - tw * 0.5f - 8.0f,
          y + 18.0f, 1.0f, C_GOLD, 220u);
  capsule(canvas, cx + tw * 0.5f + 8.0f, y + 18.0f, cx + half - 10.0f,
          y + 18.0f, 1.0f, C_GOLD, 220u);
  text_centered(canvas, false, name, cx, (int32_t)y + 6, C_GOLD, 256u);
}

static void draw_lamp_status(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_battle_t* b = &game->battle;
  char line[96];
  for (int i = 0; i < 2; ++i) {
    const h2_xianxia_rpg_rect_t* r = &game->layout.party[i];
    panel(canvas, (float)r->x, (float)r->y, (float)r->w, (float)r->h, 6.0f,
          256u);
    (void)snprintf(line, sizeof(line),
                   h2_xianxia_rpg_text[i ? H2_XIANXIA_RPG_TEXT_C4_ALLY
                                         : H2_XIANXIA_RPG_TEXT_C4_HERO],
                   (unsigned)(i ? game->ally_hp : game->hero.hp),
                   (unsigned)(i ? game->ally_hp_max : game->hero.hp_max));
    text_centered(canvas, false, line, (float)(r->x + r->w / 2), r->y, C_TEXT,
                  256u);
    if (b->state == H2_XIANXIA_RPG_BATTLE_HEAL_TARGET) {
      (void)snprintf(line, sizeof(line), "%s",
                     h2_xianxia_rpg_command_label(H2_XIANXIA_RPG_COMMAND_PILL));
    } else if (i) {
      const char* action =
          h2_xianxia_rpg_text[game->ally_hp == 0u ? H2_XIANXIA_RPG_TEXT_C4_DOWN
                              : b->ally_mend ? H2_XIANXIA_RPG_TEXT_C4_MEND
                                             : H2_XIANXIA_RPG_TEXT_C4_POLE];
      (void)snprintf(line, sizeof(line),
                     h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C4_SWITCH],
                     action);
    } else {
      (void)snprintf(line, sizeof(line),
                     h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C4_RESOURCES],
                     (unsigned)game->hero.mp, (unsigned)game->hero.pills);
    }
    text_centered(canvas, false, line, (float)(r->x + r->w / 2), r->y + LINE_PX,
                  C_GOLD, 256u);
  }
  (void)snprintf(line, sizeof(line),
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C4_LAMP],
                 (unsigned)b->lamp_hp, (unsigned)b->wave,
                 (unsigned)b->wave_turn, (unsigned)b->enemy_hp);
  const int32_t lamp_y = game->height < 300 ? 44 : 58;
  fill_rect(canvas, 8, (float)lamp_y, (float)game->width - 16, 26,
            RGB(20, 16, 24), 256u);
  text_centered(canvas, false, line, (float)game->width * 0.5f, lamp_y, C_GOLD,
                256u);
  fill_rect(canvas, 10, (float)lamp_y + 24, (float)game->width - 20, 4,
            RGB(66, 52, 44), 256u);
  if (b->lamp_max > 0u)
    fill_rect(
        canvas, 10, (float)lamp_y + 24,
        ((float)game->width - 20) * (float)b->lamp_hp / (float)b->lamp_max, 4,
        RGB(240, 174, 70), 256u);
}

static void draw_battle_static(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const h2_xianxia_rpg_battle_t* b = &game->battle;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  const int32_t w = canvas->width;
  if (game->chapter == 5u) {
    const h2_xianxia_rpg_rect_t* hero = &l->party[0];
    const h2_xianxia_rpg_rect_t* foe = &l->party[1];
    char line[96];
    panel(canvas, (float)hero->x, (float)hero->y, (float)hero->w,
          (float)hero->h, 6, 256u);
    panel(canvas, (float)foe->x, (float)foe->y, (float)foe->w, (float)foe->h, 6,
          256u);
    (void)snprintf(line, sizeof(line),
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C4_HERO],
                   (unsigned)game->hero.hp, (unsigned)game->hero.hp_max);
    text_centered(canvas, false, line, (float)(hero->x + hero->w / 2), hero->y,
                  C_TEXT, 256u);
    (void)snprintf(line, sizeof(line),
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C4_RESOURCES],
                   (unsigned)game->hero.mp, (unsigned)game->hero.pills);
    text_centered(canvas, false, line, (float)(hero->x + hero->w / 2),
                  hero->y + LINE_PX, C_GOLD, 256u);
    (void)snprintf(
        line, sizeof(line), h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C5_ENEMY],
        info->name, (unsigned)b->enemy_hp, (unsigned)b->enemy_hp_max);
    text_centered(canvas, false, line, (float)(foe->x + foe->w / 2), foe->y,
                  C_TEXT, 256u);
    bar(canvas, foe->x + 8, foe->y + LINE_PX + 4, foe->w - 16, 10, b->enemy_hp,
        b->enemy_hp_max, RGB(160, 60, 160));
  } else if (b->lamp_battle) {
    draw_lamp_status(canvas, game);
  } else {
    /* Hero status, top left. */
    const int32_t status_w = w / 2 - 20 < 300 ? w / 2 - 20 : 300;
    rounded_rect(canvas, 6.0f, 6.0f, (float)(status_w + 18),
                 (float)(2 * LINE_PX + 18), 8.0f, RGB(20, 16, 24), 190u);
    hero_status(canvas, game, 14, 10, status_w, true);
    /* Enemy status, top right. */
    const int32_t ex = w - status_w - 18;
    rounded_rect(canvas, (float)ex, 6.0f, (float)(w - 6),
                 (float)(2 * LINE_PX + 18), 8.0f, RGB(20, 16, 24), 190u);
    (void)text_at(canvas, false, info->name, ex + 10, 10,
                  b->boss ? RGB(255, 130, 110) : C_GOLD, 256u);
    bar(canvas, ex + 10, 12 + LINE_PX, status_w - 4, LINE_PX - 4, b->enemy_hp,
        b->enemy_hp_max, RGB(160, 60, 160));
  }
  if (game->chapter == 5u) {
    char goal[96];
    (void)snprintf(
        goal, sizeof(goal),
        h2_xianxia_rpg_text[b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU
                                ? H2_XIANXIA_RPG_TEXT_C5_DUEL
                                : H2_XIANXIA_RPG_TEXT_C5_WATER],
        (unsigned)(b->enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU ? b->turn
                                                            : b->enemy_attack));
    text_centered(canvas, false, goal, (float)w * 0.5f,
                  game->height < 300 ? 48 : 58, C_GOLD, 256u);
  }
  /* Lower panel: message and commands. */
  fill_rect(canvas, (float)l->panel.x, (float)l->panel.y, (float)l->panel.w,
            (float)l->panel.h, RGB(20, 16, 24), 256u);
  fill_rect(canvas, (float)l->panel.x, (float)l->panel.y, (float)l->panel.w,
            2.0f, C_GOLD, 220u);
  const h2_xianxia_rpg_rect_t* m = &l->message;
  panel(canvas, (float)m->x, (float)m->y, (float)m->w, (float)m->h, 8.0f, 230u);
  int32_t message_w = m->w - 24;
  const bool over = b->state == H2_XIANXIA_RPG_BATTLE_VICTORY ||
                    b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT ||
                    b->state == H2_XIANXIA_RPG_BATTLE_FLED;
  /* The lanterns give way to the tap hint once the battle is over. */
  if (b->ally && !over && !b->lamp_battle) {
    /* Rapport lanterns at the right end of the message box. */
    const int32_t lamp = LINE_PX - 6;
    const char* label = h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_RAPPORT];
    const int32_t label_w = h2_xianxia_rpg_text_width(false, label);
    const int32_t block =
        label_w + 6 + (int32_t)h2_xianxia_rpg_rapport_max(game) * (lamp + 4);
    const int32_t bx = m->x + m->w - 12 - block;
    const int32_t by = m->y + (m->h - LINE_PX) / 2;
    (void)text_at(canvas, false, label, bx, by, C_DIM, 240u);
    for (uint32_t i = 0; i < h2_xianxia_rpg_rapport_max(game); ++i) {
      const float cx = (float)(bx + label_w + 6) +
                       (float)i * (float)(lamp + 4) + (float)lamp * 0.5f;
      const float cy = (float)by + (float)LINE_PX * 0.5f;
      const bool lit = i < b->rapport;
      if (lit)
        glow(canvas, cx, cy, (float)lamp * 0.9f, RGB(255, 210, 110), 170u);
      rounded_rect(canvas, cx - (float)lamp * 0.32f, cy - (float)lamp * 0.42f,
                   cx + (float)lamp * 0.32f, cy + (float)lamp * 0.42f, 3.0f,
                   lit ? RGB(255, 224, 140) : RGB(70, 62, 66), 256u);
      fill_rect(canvas, cx - (float)lamp * 0.36f, cy - (float)lamp * 0.5f,
                (float)lamp * 0.72f, 2.0f, RGB(150, 100, 60), 256u);
    }
    message_w -= block + 10;
  }
  text_wrapped(canvas, b->message, m->x + 12, m->y + 8, message_w, 2, C_TEXT,
               256u, UINT32_MAX);
  const bool finished = b->state == H2_XIANXIA_RPG_BATTLE_VICTORY ||
                        b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT;
  if (finished && b->state_ms >= 450u) {
    const char* hint = h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_TAP_NEXT];
    const int32_t hw = h2_xianxia_rpg_text_width(false, hint);
    (void)text_at(canvas, false, hint, m->x + m->w - hw - 12,
                  m->y + m->h - LINE_PX - 4, C_GOLD, 230u);
  }
  const bool choosing = b->state == H2_XIANXIA_RPG_BATTLE_COMMAND ||
                        b->state == H2_XIANXIA_RPG_BATTLE_HEAL_TARGET ||
                        b->state == H2_XIANXIA_RPG_BATTLE_TARGET;
  const bool joint = h2_xianxia_rpg_joint_ready(game);
  for (int32_t i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i) {
    const h2_xianxia_rpg_rect_t* r = &l->commands[i];
    const bool available =
        h2_xianxia_rpg_command_available(game, (h2_xianxia_rpg_command_t)i);
    const bool lit = choosing && available;
    const bool joint_button = joint && i == H2_XIANXIA_RPG_COMMAND_SKILL;
    const bool pending =
        b->state == H2_XIANXIA_RPG_BATTLE_TARGET && i == (int32_t)b->pending;
    const float x0 = (float)r->x, y0 = (float)r->y;
    const float x1 = x0 + (float)r->w, y1 = y0 + (float)r->h;
    if (lit && joint_button)
      glow(canvas, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (float)r->w * 0.7f,
           RGB(150, 255, 220), 120u);
    rounded_rect(canvas, x0, y0, x1, y1, 8.0f,
                 pending               ? RGB(255, 250, 220)
                 : lit && joint_button ? RGB(150, 240, 210)
                 : lit                 ? C_GOLD
                                       : RGB(84, 76, 70),
                 256u);
    rounded_rect(canvas, x0 + 2.0f, y0 + 2.0f, x1 - 2.0f, y1 - 2.0f, 6.5f,
                 lit && joint_button ? RGB(30, 80, 76)
                 : lit               ? RGB(74, 46, 40)
                                     : RGB(44, 38, 44),
                 256u);
    if (lit)
      rounded_rect(canvas, x0 + 3.0f, y0 + 3.0f, x1 - 3.0f,
                   y0 + (float)r->h * 0.45f, 5.0f, RGB(255, 230, 180), 40u);
    const char* label =
        b->lamp_battle && i == H2_XIANXIA_RPG_COMMAND_FLEE
            ? h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C4_GUARD]
        : joint_button
            ? h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_JOINT]
            : h2_xianxia_rpg_command_label((h2_xianxia_rpg_command_t)i);
    text_centered(canvas, false, label, (x0 + x1) * 0.5f,
                  (int32_t)((y0 + y1) * 0.5f) - LINE_PX / 2,
                  lit ? C_TEXT : RGB(130, 124, 120), 256u);
    if (b->ally && i == H2_XIANXIA_RPG_COMMAND_SKILL)
      /* The sword art fills toward the joint strike. */
      for (uint32_t k = 0; k < h2_xianxia_rpg_rapport_max(game); ++k)
        disc(canvas, (x0 + x1) * 0.5f + ((float)k - 1.0f) * 9.0f, y1 - 6.0f,
             2.4f, k < b->rapport ? RGB(255, 214, 110) : RGB(90, 82, 80), 256u);
  }
  /* What to do now, under the status boxes. */
  const h2_xianxia_rpg_text_id_t hint = battle_hint(game);
  if (hint != H2_XIANXIA_RPG_TEXT_COUNT) {
    const char* line = h2_xianxia_rpg_text[hint];
    const float tw = (float)h2_xianxia_rpg_text_width(false, line);
    const float cx = (float)w * 0.5f,
                y = (b->lamp_battle || game->chapter == 5u) &&
                            game->height >= 300
                        ? 88.0f
                        : (float)(2 * LINE_PX + 24);
    rounded_rect(canvas, cx - tw * 0.5f - 14.0f, y, cx + tw * 0.5f + 14.0f,
                 y + (float)LINE_PX + 10.0f, 8.0f, C_GOLD, 230u);
    rounded_rect(canvas, cx - tw * 0.5f - 12.0f, y + 2.0f,
                 cx + tw * 0.5f + 12.0f, y + (float)LINE_PX + 8.0f, 6.5f,
                 RGB(60, 28, 26), 240u);
    text_centered(canvas, false, line, cx, (int32_t)y + 5, C_TEXT, 256u);
  }
  /* The companion's next action above her head. */
  if (b->ally && choosing && !b->lamp_battle && game->chapter != 5u) {
    const h2_xianxia_rpg_ally_action_t intent =
        h2_xianxia_rpg_ally_intent(game);
    char bubble[48];
    (void)snprintf(bubble, sizeof(bubble),
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_INTENT],
                   h2_xianxia_rpg_text[intent == H2_XIANXIA_RPG_ALLY_LANTERN
                                           ? H2_XIANXIA_RPG_TEXT_INTENT_LANTERN
                                           : H2_XIANXIA_RPG_TEXT_INTENT_POLE]);
    const float sh = (float)l->stage.h;
    const float size = fminf(sh * 0.5f, (float)w * 0.26f);
    const float ax = (float)w * 0.1f;
    const float top = sh * 0.94f - size * 1.08f;
    const float bw = (float)h2_xianxia_rpg_text_width(false, bubble) + 16.0f;
    const float bx = fmaxf(4.0f, ax - bw * 0.5f);
    const float by = top - (float)LINE_PX - 10.0f;
    rounded_rect(canvas, bx, by, bx + bw, by + (float)LINE_PX + 6.0f, 8.0f,
                 intent == H2_XIANXIA_RPG_ALLY_LANTERN ? RGB(200, 255, 236)
                                                       : RGB(240, 230, 210),
                 236u);
    triangle(canvas, ax - 6.0f, by + (float)LINE_PX + 5.0f, ax + 6.0f,
             by + (float)LINE_PX + 5.0f, ax, by + (float)LINE_PX + 12.0f,
             RGB(240, 236, 220), 236u);
    paper_centered(canvas, false, bubble, bx + bw * 0.5f, (int32_t)by + 3,
                   RGB(40, 60, 60), 256u);
  }
}

/* ---- Public renderers -------------------------------------------------- */

void h2_xianxia_rpg_render_background(canvas_t* canvas, const game_t* game) {
  unclip(canvas);
  switch (game->phase) {
    case H2_XIANXIA_RPG_PHASE_TITLE:
    case H2_XIANXIA_RPG_PHASE_CHAPTERS:
      paper_landscape(canvas, game, false, false);
      break;
    case H2_XIANXIA_RPG_PHASE_ENDING:
      paper_landscape(canvas, game, true, game->chapter >= 2u);
      break;
    case H2_XIANXIA_RPG_PHASE_PROLOGUE:
      paper_landscape(canvas, game, true, game->prologue_chapter >= 2u);
      break;
    case H2_XIANXIA_RPG_PHASE_EXPLORE:
      map_background(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_BATTLE:
      battle_backdrop(canvas, game);
      break;
  }
}

static void draw_c6_ritual(canvas_t* canvas, const game_t* game) {
  const int32_t w = game->width, h = game->height;
  const bool compact = h < 360;
  const bool both = (game->facts & 3u) == 3u;
  const uint32_t meter = game->flags & 15u;
  const uint32_t streak = (game->flags >> 4) & 7u;
  const int32_t top = compact ? 8 : 32;
  fill_rect(canvas, 0, 0, (float)w, (float)h, RGB(23, 34, 45), 256u);
  char line[96];
  (void)snprintf(line, sizeof(line),
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C6_RITUAL],
                 (unsigned)streak);
  text_centered(canvas, false, line, (float)w * .5f, top, C_GOLD, 256u);
  const int32_t gy = top + (compact ? 40 : 110);
  const int32_t cell = (w - 48) / 9, gx = (w - cell * 9) / 2;
  for (uint32_t i = 0; i <= 8u; ++i) {
    const bool steady = i >= (both ? 2u : 3u) && i <= (both ? 6u : 5u);
    const uint16_t color = steady   ? RGB(100, 177, 142)
                           : i < 4u ? RGB(75, 139, 188)
                                    : RGB(209, 115, 75);
    fill_rect(canvas, (float)(gx + (int32_t)i * cell), (float)gy,
              (float)(cell - 2), 18, color, 256u);
    if (i == meter)
      diamond(canvas, (float)(gx + (int32_t)i * cell + cell / 2),
              (float)(gy - 8), 6, C_GOLD, 256u);
    char n[4];
    (void)snprintf(n, sizeof(n), "%u", (unsigned)i);
    text_centered(canvas, false, n, (float)(gx + (int32_t)i * cell + cell / 2),
                  gy + 20, C_TEXT, 256u);
  }
  (void)snprintf(line, sizeof(line),
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C6_RULE],
                 both ? 2u : 3u, both ? 6u : 5u);
  text_centered(canvas, false, line, (float)w * .5f, gy + 47, C_TEXT, 256u);
  const h2_xianxia_rpg_text_id_t next =
      game->turns_taken % 6u == 2u         ? H2_XIANXIA_RPG_TEXT_C6_WISP
      : h2_xianxia_rpg_c6_breath(game) > 0 ? H2_XIANXIA_RPG_TEXT_C6_BREATH_HOT
                                           : H2_XIANXIA_RPG_TEXT_C6_BREATH_COLD;
  text_centered(canvas, false, h2_xianxia_rpg_text[next], (float)w * .5f,
                gy + 77, C_GOLD, 256u);
  if (!compact) {
    draw_person(canvas, H2_XIANXIA_RPG_ACTOR_JIANGLI, (float)w * .5f,
                (float)gy - 28, 50, H2_XIANXIA_RPG_FACING_DOWN, 0, 0);
    text_centered(canvas, false,
                  h2_xianxia_rpg_text[both ? H2_XIANXIA_RPG_TEXT_C6_CHARM
                                           : H2_XIANXIA_RPG_TEXT_C6_PAUSE],
                  (float)w * .5f, gy + 115, C_DIM, 256u);
  }
  for (unsigned i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i)
    button(canvas, &game->layout.commands[i],
           h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C6_COOL + i], true);
}

void h2_xianxia_rpg_render_static(canvas_t* canvas, const game_t* game) {
  unclip(canvas);
  switch (game->phase) {
    case H2_XIANXIA_RPG_PHASE_TITLE:
      draw_title(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_ENDING:
      draw_ending(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_CHAPTERS:
      draw_chapters(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_PROLOGUE:
      draw_prologue(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_EXPLORE:
      if (game->stage == H2_XIANXIA_RPG_STAGE_C6_RITUAL) {
        draw_c6_ritual(canvas, game);
        break;
      }
      draw_hud(canvas, game);
      draw_banner(canvas, game);
      draw_dialogue(canvas, game);
      if (game->chapter == 7u && !game->dialogue.active) {
        const bool question =
            game->stage >= H2_XIANXIA_RPG_STAGE_C7_QUESTION1 &&
            game->stage <= H2_XIANXIA_RPG_STAGE_C7_QUESTION3;
        const uint32_t q =
            question
                ? (uint32_t)(game->stage - H2_XIANXIA_RPG_STAGE_C7_QUESTION1)
                : 0u;
        for (unsigned i = 0; i < 2u; ++i)
          button(canvas, &game->layout.escort_actions[i],
                 h2_xianxia_rpg_text[question ? H2_XIANXIA_RPG_TEXT_C7_A1 +
                                                    2u * q + i
                                     : i      ? H2_XIANXIA_RPG_TEXT_C7_TALK
                                              : H2_XIANXIA_RPG_TEXT_C7_HELP],
                 true);
      }
      if (game->chapter == 6u && !game->dialogue.active) {
        const bool hut = game->map == H2_XIANXIA_RPG_MAP_HERB_HUT;
        button(canvas, &game->layout.escort_actions[0],
               h2_xianxia_rpg_text[hut ? H2_XIANXIA_RPG_TEXT_C6_HOT
                                       : H2_XIANXIA_RPG_TEXT_C6_BACK],
               game->stage != H2_XIANXIA_RPG_STAGE_C6_ARRIVE);
        button(canvas, &game->layout.escort_actions[1],
               h2_xianxia_rpg_text[hut ? H2_XIANXIA_RPG_TEXT_C6_COLD
                                       : H2_XIANXIA_RPG_TEXT_C6_HELP],
               game->stage != H2_XIANXIA_RPG_STAGE_C6_ARRIVE);
      }
      if (game->chapter == 5u && !game->dialogue.active) {
        const bool choice = game->stage == H2_XIANXIA_RPG_STAGE_C5_CHOICE;
        if (choice || (game->map == H2_XIANXIA_RPG_MAP_OLD_CITY &&
                       game->stage < H2_XIANXIA_RPG_STAGE_C5_RESCUE))
          button(canvas, &game->layout.escort_actions[0],
                 h2_xianxia_rpg_text[choice ? H2_XIANXIA_RPG_TEXT_C5_TAKE
                                            : H2_XIANXIA_RPG_TEXT_C5_OTTER],
                 true);
        button(canvas, &game->layout.escort_actions[1],
               h2_xianxia_rpg_text[choice ? H2_XIANXIA_RPG_TEXT_C5_GIVE
                                          : H2_XIANXIA_RPG_TEXT_C5_HELP],
               true);
      }
      if (game->chapter == 4u && !game->dialogue.active) {
        button(canvas, &game->layout.escort_actions[0],
               h2_xianxia_rpg_text[game->flags & H2_XIANXIA_RPG_FLAG_C4_COVER
                                       ? H2_XIANXIA_RPG_TEXT_C4_COVER
                                       : H2_XIANXIA_RPG_TEXT_C4_SCOUT],
               true);
        button(canvas, &game->layout.escort_actions[1],
               h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_C4_HELP], true);
      }
      break;
    case H2_XIANXIA_RPG_PHASE_BATTLE:
      draw_battle_static(canvas, game);
      break;
  }
}

static void draw_particles(canvas_t* canvas, const game_t* game) {
  const float u = game->unit;
  for (unsigned i = 0; i < H2_XIANXIA_RPG_MAX_PARTICLES; ++i) {
    const h2_xianxia_rpg_particle_t* p = &game->particles[i];
    if (!p->active) continue;
    switch (p->kind) {
      case 0:
        ellipse(canvas, p->x, p->y, 5.0f * u, 1.8f * u, sinf(p->phase) * 1.2f,
                RGB(96, 160, 70), 230u);
        break;
      case 1:
        ellipse(canvas, p->x, p->y, 3.6f * u, 2.2f * u, p->phase,
                RGB(236, 150, 170), 230u);
        break;
      case 4:
        capsule(canvas, p->x, p->y, p->x - 2.0f * u, p->y + 14.0f * u, 0.7f * u,
                RGB(180, 216, 236), 170u);
        break;
      case 3:
        glow(canvas, p->x, p->y, 24.0f * u, RGB(244, 246, 248),
             (uint32_t)(70.0f + 30.0f * sinf(p->phase)));
        break;
      default: {
        const float pulse = 0.5f + 0.5f * sinf(p->phase * 2.0f);
        glow(canvas, p->x, p->y, 7.0f * u, RGB(170, 140, 255),
             (uint32_t)(60.0f + 90.0f * pulse));
        disc(canvas, p->x, p->y, 1.4f * u, RGB(230, 220, 255), 230u);
        break;
      }
    }
  }
}

typedef struct sprite {
  int32_t row;
  float x, feet;
  int32_t actor;
  h2_xianxia_rpg_facing_t facing;
  float walk, bob;
  bool quest;
} sprite_t;

static bool quest_actor(const game_t* game, h2_xianxia_rpg_actor_t actor) {
  switch (game->stage) {
    case H2_XIANXIA_RPG_STAGE_MEET_ELDER:
      return actor == H2_XIANXIA_RPG_ACTOR_ELDER;
    case H2_XIANXIA_RPG_STAGE_SEEK_DAOIST:
      return actor == H2_XIANXIA_RPG_ACTOR_DAOIST ||
             (actor == H2_XIANXIA_RPG_ACTOR_HERBALIST &&
              !(game->flags & H2_XIANXIA_RPG_FLAG_PILLS_GIVEN));
    case H2_XIANXIA_RPG_STAGE_TO_SHRINE:
      return actor == H2_XIANXIA_RPG_ACTOR_DRAGON;
    case H2_XIANXIA_RPG_STAGE_RETURN:
      return actor == H2_XIANXIA_RPG_ACTOR_ELDER;
    case H2_XIANXIA_RPG_STAGE_C2_ARRIVE:
    case H2_XIANXIA_RPG_STAGE_C2_RETURN:
      return actor == H2_XIANXIA_RPG_ACTOR_FERRYMASTER;
    case H2_XIANXIA_RPG_STAGE_C2_FIND_JIANGLI:
      return actor == H2_XIANXIA_RPG_ACTOR_JIANGLI;
    case H2_XIANXIA_RPG_STAGE_C6_ARRIVE:
    case H2_XIANXIA_RPG_STAGE_C6_RECOVERED:
      return actor == H2_XIANXIA_RPG_ACTOR_XUN;
    case H2_XIANXIA_RPG_STAGE_C5_ARRIVE:
    case H2_XIANXIA_RPG_STAGE_C5_RETURN:
      return actor == H2_XIANXIA_RPG_ACTOR_BOAT_DOCTOR;
    case H2_XIANXIA_RPG_STAGE_C5_TOWER:
    case H2_XIANXIA_RPG_STAGE_C5_RESCUE:
      return actor == H2_XIANXIA_RPG_ACTOR_WEN_GRANDMA;
    case H2_XIANXIA_RPG_STAGE_C5_DUEL:
      return actor == H2_XIANXIA_RPG_ACTOR_WENZHOU;
    case H2_XIANXIA_RPG_STAGE_C4_HOME:
      return actor == H2_XIANXIA_RPG_ACTOR_ELDER &&
             !(game->flags & H2_XIANXIA_RPG_FLAG_C4_JOINED);
    case H2_XIANXIA_RPG_STAGE_C4_RETURN:
      return actor == H2_XIANXIA_RPG_ACTOR_ELDER;
    case H2_XIANXIA_RPG_STAGE_C3_HALL:
      return actor == H2_XIANXIA_RPG_ACTOR_SWORD_SOUL;
    case H2_XIANXIA_RPG_STAGE_C3_RETURN:
      return actor == H2_XIANXIA_RPG_ACTOR_SHIYAN;
    case H2_XIANXIA_RPG_STAGE_C2_WRECK:
      return actor == H2_XIANXIA_RPG_ACTOR_WRECK_GUARD;
    default:
      return false;
  }
}

/* The object the current objective points at on this map, if any. */
static bool quest_tile(const game_t* game, int32_t* out_x, int32_t* out_y) {
  char wanted = '\0';
  const uint32_t flags = game->flags;
  switch (game->stage) {
    case H2_XIANXIA_RPG_STAGE_C6_HERBS:
      wanted = 'm';
      break;
    case H2_XIANXIA_RPG_STAGE_C6_READY:
      wanted = game->map == H2_XIANXIA_RPG_MAP_HERB_HUT ? 'R' : 'm';
      break;
    case H2_XIANXIA_RPG_STAGE_C5_PAGE:
      wanted = flags & H2_XIANXIA_RPG_FLAG_C5_HIGH ? 'B' : 'p';
      break;
    case H2_XIANXIA_RPG_STAGE_C5_TOWER:
      if (!(flags & H2_XIANXIA_RPG_FLAG_C5_HIGH)) wanted = 'B';
      break;
    case H2_XIANXIA_RPG_STAGE_C5_RESCUE:
    case H2_XIANXIA_RPG_STAGE_C5_ESCORT:
      wanted = 'B';
      break;
    case H2_XIANXIA_RPG_STAGE_C4_HOME:
      if (flags & H2_XIANXIA_RPG_FLAG_C4_JOINED) wanted = 'F';
      break;
    case H2_XIANXIA_RPG_STAGE_C4_ESCORT:
      wanted = !(flags & 1u) ? '1' : !(flags & 2u) ? '2' : '3';
      break;
    case H2_XIANXIA_RPG_STAGE_C4_GUARD:
    case H2_XIANXIA_RPG_STAGE_C4_RETURN:
      wanted = '3';
      break;
    case H2_XIANXIA_RPG_STAGE_C3_ASCEND:
      wanted = 'n';
      break;
    case H2_XIANXIA_RPG_STAGE_C3_ARRAY:
      wanted = !(flags & H2_XIANXIA_RPG_FLAG_C3_STELE1)   ? 'a'
               : !(flags & H2_XIANXIA_RPG_FLAG_C3_STELE2) ? 'b'
                                                          : 'O';
      break;
    case H2_XIANXIA_RPG_STAGE_C3_HALL:
      if (!(flags & H2_XIANXIA_RPG_FLAG_C3_STELE3)) wanted = 'c';
      break;
    case H2_XIANXIA_RPG_STAGE_C2_TO_REEDS:
      wanted = !(flags & H2_XIANXIA_RPG_FLAG_C2_POST1)   ? '1'
               : !(flags & H2_XIANXIA_RPG_FLAG_C2_POST2) ? '2'
                                                         : 'U';
      break;
    case H2_XIANXIA_RPG_STAGE_C2_TO_WRECK:
      wanted = 'U';
      break;
    case H2_XIANXIA_RPG_STAGE_C2_WRECK:
      if (flags & H2_XIANXIA_RPG_FLAG_C2_GUARD) wanted = 'Y';
      break;
    default:
      break;
  }
  if (wanted == '\0') return false;
  for (int32_t row = 0; row < ROWS; ++row)
    for (int32_t col = 0; col < COLS; ++col)
      if (h2_xianxia_rpg_tile(game->map, col, row) == wanted) {
        *out_x = col;
        *out_y = row;
        return true;
      }
  return false;
}

static void quest_marker(canvas_t* canvas, float x, float top, float t,
                         float idle) {
  const float lift = sinf(idle * 2.0f) * t * 0.05f;
  glow(canvas, x, top + lift, t * 0.45f, RGB(255, 214, 110), 150u);
  diamond(canvas, x, top + lift, t * 0.23f, RGB(60, 30, 10), 230u);
  diamond(canvas, x, top + lift, t * 0.18f, C_GOLD, 256u);
  diamond(canvas, x - t * 0.03f, top + lift - t * 0.06f, t * 0.06f,
          RGB(255, 250, 222), 230u);
}

static void explore_scene(canvas_t* canvas, const game_t* game) {
  if (game->stage == H2_XIANXIA_RPG_STAGE_C6_RITUAL) return;
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float t = (float)l->tile;
  int32_t clip_bottom = l->map_y + ROWS * l->tile;
  if (!l->stacked && game->dialogue.active && l->dialog.y < clip_bottom)
    clip_bottom = l->dialog.y;
  if ((game->chapter >= 4u && game->chapter <= 7u) && !game->dialogue.active &&
      l->escort_actions[0].y < clip_bottom)
    clip_bottom = l->escort_actions[0].y;
  clip_to(canvas, l->map_x, l->map_y, l->map_x + COLS * l->tile, clip_bottom);

  if (game->marker_ms > 0u && !game->dialogue.active) {
    const float k = (float)game->marker_ms / 600.0f;
    const float cx = (float)l->map_x + ((float)game->marker_x + 0.5f) * t;
    const float cy = (float)l->map_y + ((float)game->marker_y + 0.5f) * t;
    ellipse(canvas, cx, cy + t * 0.2f, t * (0.4f - 0.15f * k), t * 0.15f, 0.0f,
            C_GOLD, (uint32_t)(200.0f * k));
    ellipse(canvas, cx, cy + t * 0.2f, t * (0.3f - 0.12f * k), t * 0.1f, 0.0f,
            RGB(60, 100, 50), (uint32_t)(200.0f * k));
  }

  sprite_t sprites[H2_XIANXIA_RPG_MAX_NPCS + 3];
  size_t count = 0;
  h2_xianxia_rpg_npc_t npcs[H2_XIANXIA_RPG_MAX_NPCS];
  const size_t npc_count = h2_xianxia_rpg_npcs(game, game->map, npcs);
  const float idle = (float)(game->time_ms % 1600u) * (2.0f * PI / 1600.0f);
  for (size_t i = 0; i < npc_count; ++i) {
    sprites[count++] = (sprite_t){
        .row = npcs[i].y,
        .x = (float)l->map_x + ((float)npcs[i].x + 0.5f) * t,
        .feet = (float)l->map_y + ((float)npcs[i].y + 0.92f) * t,
        .actor = npcs[i].actor,
        .facing = (h2_xianxia_rpg_facing_t)npcs[i].facing,
        .walk = 0.0f,
        .bob = sinf(idle + (float)i) * t * 0.015f,
        .quest = quest_actor(game, (h2_xianxia_rpg_actor_t)npcs[i].actor)};
  }
  float progress = game->moving
                       ? (float)game->move_ms / (float)H2_XIANXIA_RPG_STEP_MS
                       : 1.0f;
  progress = clampf(progress, 0.0f, 1.0f);
  const float hx =
      (float)game->from_x + (float)(game->tx - game->from_x) * progress;
  const float hy =
      (float)game->from_y + (float)(game->ty - game->from_y) * progress;
  sprites[count++] = (sprite_t){
      .row = game->ty,
      .x = (float)l->map_x + (hx + 0.5f) * t,
      .feet = (float)l->map_y + (hy + 0.92f) * t,
      .actor = H2_XIANXIA_RPG_ACTOR_HERO,
      .facing = game->facing,
      .walk = game->moving ? ((float)game->walk_steps + progress) * PI : 0.0f,
      .bob = game->moving
                 ? -fabsf(sinf(((float)game->walk_steps + progress) * PI)) * t *
                       0.04f
                 : 0.0f,
      .quest = false};
  if (game->ally_visible) {
    const float ax = (float)game->ally_from_x +
                     (float)(game->ally_x - game->ally_from_x) * progress;
    const float ay = (float)game->ally_from_y +
                     (float)(game->ally_y - game->ally_from_y) * progress;
    const bool same = game->ally_x == game->tx && game->ally_y == game->ty;
    sprites[count++] = (sprite_t){
        .row = game->ally_y,
        .x = (float)l->map_x + (ax + 0.5f) * t + (same ? -t * 0.3f : 0.0f),
        /* Just behind the hero when both share a tile. */
        .feet = (float)l->map_y + (ay + 0.92f) * t - (same ? 0.5f : 0.0f),
        .actor = H2_XIANXIA_RPG_ACTOR_JIANGLI,
        .facing = game->facing,
        .walk = game->moving ? ((float)game->walk_steps + progress) * PI : 0.0f,
        .bob = 0.0f,
        .quest = false};
  }
  if (game->chapter == 5u && game->stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT) {
    sprites[count++] = (sprite_t){.row = game->ty,
                                  .x = (float)l->map_x + (hx + 0.27f) * t,
                                  .feet = (float)l->map_y + (hy + 0.68f) * t,
                                  .actor = H2_XIANXIA_RPG_ACTOR_WEN_GRANDMA,
                                  .facing = game->facing};
  }
  /* Painter's order by feet. */
  for (size_t i = 1; i < count; ++i)
    for (size_t j = i; j > 0 && sprites[j - 1].feet > sprites[j].feet; --j) {
      const sprite_t swap = sprites[j];
      sprites[j] = sprites[j - 1];
      sprites[j - 1] = swap;
    }
  for (size_t i = 0; i < count; ++i) {
    const sprite_t* s = &sprites[i];
    if (s->actor == H2_XIANXIA_RPG_ACTOR_DRAGON)
      draw_map_dragon(canvas, s->x, s->feet - t * 0.45f, t, game->time_ms);
    else if (s->actor == H2_XIANXIA_RPG_ACTOR_WRECK_GUARD)
      draw_map_guard(canvas, s->x, s->feet, t, game->time_ms);
    else
      draw_person(canvas, (h2_xianxia_rpg_actor_t)s->actor, s->x, s->feet, t,
                  s->facing, s->walk, s->bob);
    if (s->quest)
      quest_marker(
          canvas, s->x,
          s->feet -
              t * (s->actor == H2_XIANXIA_RPG_ACTOR_DRAGON ? 1.55f : 1.4f),
          t, idle);
  }
  int32_t qx, qy;
  if (quest_tile(game, &qx, &qy))
    quest_marker(canvas, (float)l->map_x + ((float)qx + 0.5f) * t,
                 (float)l->map_y + ((float)qy - 0.35f) * t, t, idle);
  if (game->chapter == 3u && game->map == H2_XIANXIA_RPG_MAP_SWORD_HALL &&
      game->dialogue.active &&
      game->dialogue.script ==
          h2_xianxia_rpg_script(H2_XIANXIA_RPG_SCRIPT_C3_REVEAL) &&
      game->dialogue.index >= 3u && game->dialogue.index <= 5u) {
    const float wx = (float)l->map_x + 11.5f * t,
                wy = (float)l->map_y + 1.8f * t;
    draw_person(canvas, H2_XIANXIA_RPG_ACTOR_WENZHOU, wx, wy, t,
                H2_XIANXIA_RPG_FACING_DOWN, 0, 0);
    triangle(canvas, wx - t * 0.4f, wy - t * 0.8f, wx + t * 0.4f, wy - t * 0.8f,
             wx, wy - t * 1.15f, RGB(150, 120, 82), 256u);
  }
  draw_particles(canvas, game);
  unclip(canvas);
}

static void battle_scene(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const h2_xianxia_rpg_battle_t* b = &game->battle;
  const float w = (float)canvas->width;
  const float sh = (float)l->stage.h;
  const float u = game->unit;
  const bool compact = b->lamp_battle || game->chapter == 5u;
  if (compact && l->stage.h <= 122) return;
  const float s = compact && sh < 260.0f ? (sh - 122.0f) * 0.65f
                                         : fminf(sh * 0.5f, w * 0.26f);
  const float lamp_y = b->lamp_battle && sh < 260.0f
                           ? 122.0f + (sh - 122.0f) * 0.15f
                           : sh * 0.62f;
  clip_to(canvas, 0, compact ? 122 : 2 * LINE_PX + 20, canvas->width,
          l->stage.h);
  float ex = w * 0.68f,
        ey = compact && sh < 260.0f ? (122.0f + sh) * 0.5f : sh * 0.6f;
  float hx = w * 0.26f;
  float ax = w * 0.1f;
  const float hfeet = sh * 0.94f;
  const float duration = (float)H2_XIANXIA_RPG_ACT_MS;
  const float k = clampf((float)b->state_ms / (duration * 0.6f), 0.0f, 1.0f);
  const float ally_k =
      clampf((float)b->state_ms / ((float)H2_XIANXIA_RPG_ALLY_ACT_MS * 0.6f),
             0.0f, 1.0f);
  const bool ally_acting = b->state == H2_XIANXIA_RPG_BATTLE_ALLY_ACT;
  if (b->split) {
    const h2_xianxia_rpg_rect_t* real = &l->shadows[b->real_shadow & 1u];
    ex = (float)real->x + (float)real->w * 0.5f;
  }
  if (ally_acting && b->ally_action == H2_XIANXIA_RPG_ALLY_POLE)
    ax += sinf(ally_k * PI) * (ex - ax) * 0.4f;
  if (b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT &&
      (b->command == H2_XIANXIA_RPG_COMMAND_ATTACK ||
       b->command == H2_XIANXIA_RPG_COMMAND_SKILL))
    hx += sinf(k * PI) * (ex - hx) * 0.45f;
  const float victim_x = b->lamp_battle && b->hit_target == 2u
                             ? w * 0.39f + s * 0.325f
                         : b->lamp_battle && b->hit_target == 1u ? ax
                                                                 : hx;
  if (b->state == H2_XIANXIA_RPG_BATTLE_ENEMY_ACT && b->hit_count > 0u)
    ex -= sinf(k * PI) * (ex - victim_x) * 0.3f;
  const float shake = ((game->time_ms / 30u) % 2u ? 1.0f : -1.0f) * 3.0f * u;
  if (b->enemy_flash_ms > 0u) ex += shake;
  if (b->hero_flash_ms > 0u) hx += shake;

  if (b->lamp_battle)
    lamp_post(canvas, w * 0.39f, lamp_y, s * 0.65f, b->lamp_hp > 0u);
  /* Enemy, or its dispersal after defeat. */
  if (b->state != H2_XIANXIA_RPG_BATTLE_VICTORY && b->split) {
    /* Two shadows; the companion's lantern rings the real one. */
    const bool choosing = b->state == H2_XIANXIA_RPG_BATTLE_TARGET;
    const float pulse = 0.5f + 0.5f * sinf((float)(game->time_ms % 1200u) *
                                           (2.0f * PI / 1200.0f));
    for (uint32_t i = 0; i < 2u; ++i) {
      const h2_xianxia_rpg_rect_t* r = &l->shadows[i];
      const float cx = (float)r->x + (float)r->w * 0.5f;
      const bool real = i == (uint32_t)(b->real_shadow & 1u);
      if (real) {
        glow(canvas, cx, ey + s * 0.52f, s * 0.55f, RGB(200, 255, 236), 200u);
        ellipse(canvas, cx, ey + s * 0.5f, s * 0.36f, s * 0.08f, 0.0f,
                RGB(230, 255, 240), 220u);
      }
      draw_enemy(canvas, (h2_xianxia_rpg_enemy_t)b->enemy, cx, ey, s * 0.8f,
                 game->time_ms + i * 700u, b->charging && real);
      if (choosing)
        ellipse(canvas, cx, ey + s * 0.5f, s * 0.42f, s * 0.1f, 0.0f,
                RGB(255, 240, 200), (uint32_t)(60.0f + 80.0f * pulse));
      if (real && b->enemy_flash_ms > 0u)
        glow(canvas, cx, ey, s * 0.5f, RGB(255, 255, 255),
             b->enemy_flash_ms * 200u / H2_XIANXIA_RPG_FLASH_MS);
    }
  } else if (b->state != H2_XIANXIA_RPG_BATTLE_VICTORY) {
    if (b->enemy == H2_XIANXIA_RPG_ENEMY_SWORD_SOUL)
      draw_sword_soul(canvas, ex, ey, s, b->sword_stance, game->time_ms);
    else
      draw_enemy(canvas, (h2_xianxia_rpg_enemy_t)b->enemy, ex, ey, s,
                 game->time_ms, b->charging);
    if (b->veiled) {
      /* The veil: drifting fog over a blurred shape. */
      for (uint32_t i = 0; i < 5u; ++i) {
        const float a = (float)i * 1.3f +
                        (float)(game->time_ms % 6000u) * (2.0f * PI / 6000.0f);
        glow(canvas, ex + cosf(a) * s * 0.25f, ey + sinf(a) * s * 0.18f,
             s * 0.45f, RGB(236, 240, 244), 150u);
      }
    }
    if (b->enemy_flash_ms > 0u)
      glow(canvas, ex, ey, s * 0.6f, RGB(255, 255, 255),
           b->enemy_flash_ms * 200u / H2_XIANXIA_RPG_FLASH_MS);
  } else if (b->state_ms < 900u) {
    const float t = (float)b->state_ms / 900.0f;
    for (uint32_t i = 0; i < 12u; ++i) {
      const float a = (float)i * (2.0f * PI / 12.0f);
      disc(canvas, ex + cosf(a) * s * 0.6f * t,
           ey + sinf(a) * s * 0.4f * t - s * 0.3f * t,
           s * 0.05f * (1.0f - t) + 1.0f, RGB(240, 230, 255),
           (uint32_t)(240.0f * (1.0f - t)));
    }
  }

  /* The companion behind the hero, in front of him while she acts. */
  const float ally_bob =
      sinf((float)(game->time_ms % 2200u) * (PI / 1100.0f)) * s * 0.012f;
  const float ally_walk =
      ally_acting && b->ally_action == H2_XIANXIA_RPG_ALLY_POLE
          ? ally_k * PI * 2.0f
          : 0.0f;
  if (b->ally && !ally_acting)
    draw_person(canvas, H2_XIANXIA_RPG_ACTOR_JIANGLI, ax, hfeet, s * 0.85f,
                H2_XIANXIA_RPG_FACING_RIGHT, ally_walk, ally_bob);
  if (b->ally && b->split && b->state != H2_XIANXIA_RPG_BATTLE_VICTORY) {
    /* Her lantern light falls on the real shadow. */
    const h2_xianxia_rpg_rect_t* real = &l->shadows[b->real_shadow & 1u];
    const float rx = (float)real->x + (float)real->w * 0.5f;
    capsule(canvas, ax - s * 0.2f, hfeet - s * 0.4f, rx, ey + s * 0.45f,
            s * 0.05f, RGB(210, 255, 236), 60u);
  }
  if (b->ally) {
    if (ally_acting && b->ally_action == H2_XIANXIA_RPG_ALLY_LANTERN) {
      const float fade = sinf(ally_k * PI);
      const float lx = ax - s * 0.2f, ly = hfeet - s * 0.4f;
      glow(canvas, lx, ly, s * (0.4f + 0.3f * fade), RGB(200, 255, 236),
           (uint32_t)(220.0f * fade));
      const float to_x = b->lamp_battle ? w * 0.39f + s * 0.325f : ex;
      const float to_y = b->lamp_battle ? lamp_y + s * 0.18f : ey;
      capsule(canvas, lx, ly, to_x, to_y, s * 0.08f, RGB(220, 255, 240),
              (uint32_t)(110.0f * fade));
      glow(canvas, to_x, to_y, s * 0.7f, RGB(230, 255, 245),
           (uint32_t)(160.0f * fade));
    }
  }
  /* Hero, side view facing the enemy. */
  const bool fallen = b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT;
  if (b->steadied ||
      (ally_acting && b->ally_action == H2_XIANXIA_RPG_ALLY_STEADY))
    ellipse(canvas, hx, hfeet - s * 0.45f, s * 0.42f, s * 0.6f, 0.0f,
            RGB(150, 240, 220), 70u);
  if (b->guarding)
    glow(canvas, hx, hfeet - s * 0.5f, s * 0.6f, RGB(120, 180, 255), 110u);
  draw_person(
      canvas, H2_XIANXIA_RPG_ACTOR_HERO, hx,
      hfeet + (fallen ? s * 0.12f : 0.0f), s * 1.0f,
      H2_XIANXIA_RPG_FACING_RIGHT,
      b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT ? k * PI * 2.0f : 0.0f,
      sinf((float)(game->time_ms % 2000u) * (PI / 1000.0f)) * s * 0.01f);
  if (b->hero_flash_ms > 0u)
    glow(canvas, hx, hfeet - s * 0.5f, s * 0.5f, RGB(255, 80, 60),
         b->hero_flash_ms * 180u / H2_XIANXIA_RPG_FLASH_MS);
  if (fallen) glow(canvas, hx, hfeet - s * 0.4f, s * 0.6f, C_SHADOW, 150u);
  if (b->ally && ally_acting)
    draw_person(canvas, H2_XIANXIA_RPG_ACTOR_JIANGLI, ax, hfeet + s * 0.02f,
                s * 0.85f, H2_XIANXIA_RPG_FACING_RIGHT, ally_walk, ally_bob);

  /* Pole strike from the companion. */
  if (ally_acting && b->ally_action == H2_XIANXIA_RPG_ALLY_POLE &&
      b->hit_count > 0u) {
    const int32_t dt = (int32_t)b->state_ms - (int32_t)H2_XIANXIA_RPG_HIT_MS;
    if (dt > -60 && dt < 200) {
      const float fade = 1.0f - clampf((float)dt / 200.0f, 0.0f, 1.0f);
      capsule(canvas, ex - s * 0.35f, ey + s * 0.1f, ex + s * 0.2f,
              ey - s * 0.12f, s * 0.04f, RGB(220, 200, 140),
              (uint32_t)(220.0f * fade));
    }
  }
  /* Sword light at each hit. */
  if (b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT) {
    for (uint32_t i = 0; i < b->hit_count; ++i) {
      const int32_t at =
          (int32_t)(H2_XIANXIA_RPG_HIT_MS + i * H2_XIANXIA_RPG_HIT_GAP_MS);
      const int32_t dt = (int32_t)b->state_ms - at;
      if (dt < -60 || dt > 180) continue;
      const float fade = 1.0f - clampf((float)dt / 180.0f, 0.0f, 1.0f);
      const float dir = i % 2u ? -1.0f : 1.0f;
      const uint16_t color = b->joint ? RGB(170, 255, 226)
                             : b->command == H2_XIANXIA_RPG_COMMAND_SKILL
                                 ? RGB(140, 240, 255)
                                 : RGB(255, 246, 210);
      if (b->joint)
        glow(canvas, ex, ey, s * 0.8f, RGB(200, 255, 236),
             (uint32_t)(200.0f * fade));
      const float reach = s * (b->special ? 0.6f : 0.45f);
      capsule(canvas, ex - reach, ey - reach * dir * 0.8f, ex + reach,
              ey + reach * dir * 0.8f, s * 0.05f, color,
              (uint32_t)(130.0f * fade));
      capsule(canvas, ex - reach, ey - reach * dir * 0.8f, ex + reach,
              ey + reach * dir * 0.8f, s * 0.018f, RGB(255, 255, 255),
              (uint32_t)(250.0f * fade));
    }
    if (b->command == H2_XIANXIA_RPG_COMMAND_PILL && b->state_ms < 700u) {
      const float t = (float)b->state_ms / 700.0f;
      for (uint32_t i = 0; i < 6u; ++i)
        disc(canvas, hx + s * (0.3f * hash_unit(i, 3u) - 0.15f),
             hfeet - s * (0.2f + 0.8f * t + 0.2f * hash_unit(i, 5u)),
             s * 0.03f + 1.0f, RGB(140, 240, 140),
             (uint32_t)(230.0f * (1.0f - t)));
    }
  }
  /* Enemy specials burst at the hero. */
  if (b->state == H2_XIANXIA_RPG_BATTLE_ENEMY_ACT && b->special) {
    const int32_t dt = (int32_t)b->state_ms - (int32_t)H2_XIANXIA_RPG_HIT_MS;
    if (dt > -120 && dt < 300) {
      const float t = clampf(((float)dt + 120.0f) / 420.0f, 0.0f, 1.0f);
      const h2_xianxia_rpg_enemy_info_t* info =
          h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
      if (b->boss) {
        for (uint32_t i = 0; i < 3u; ++i) {
          const float y = hfeet - s * (0.2f + 0.25f * (float)i);
          const float x = ex - (ex - victim_x + s * 0.4f) * t;
          capsule(canvas, x, y, x + s * 0.5f, y - s * 0.12f, s * 0.08f,
                  RGB(30, 30, 50), (uint32_t)(220.0f * (1.0f - t * 0.4f)));
          capsule(canvas, x, y - s * 0.04f, x + s * 0.3f, y - s * 0.1f,
                  s * 0.02f, RGB(140, 120, 200), 200u);
        }
      } else {
        for (uint32_t i = 0; i < 8u; ++i) {
          const float a = (float)i * (2.0f * PI / 8.0f);
          disc(canvas, hx + cosf(a) * s * 0.45f * t,
               hfeet - s * 0.5f + sinf(a) * s * 0.45f * t, s * 0.05f,
               info->accent, (uint32_t)(230.0f * (1.0f - t)));
        }
      }
    }
  }
  unclip(canvas);

  /* Damage and healing numbers float upward. */
  for (unsigned i = 0; i < H2_XIANXIA_RPG_MAX_POPUPS; ++i) {
    const h2_xianxia_rpg_popup_t* p = &b->popups[i];
    if (!p->active || (b->lamp_battle && l->stage.h < 170)) continue;
    const float t = (float)p->age_ms / (float)H2_XIANXIA_RPG_POPUP_MS;
    char value[16];
    (void)snprintf(value, sizeof(value), p->heal ? "+%u" : "%u",
                   (unsigned)p->value);
    const float owner_x = p->target == 2u   ? w * 0.39f + s * 0.325f
                          : p->target == 1u ? ax
                                            : hx;
    const float px = (p->on_enemy ? ex : owner_x) + (float)p->slot * s * 0.18f;
    float py = (p->on_enemy       ? ey - s * 0.5f
                : p->target == 2u ? lamp_y
                                  : hfeet - s * 1.1f) -
               t * s * 0.35f;
    if (b->lamp_battle) py = fmaxf(py, 126.0f);
    const uint16_t color = p->heal       ? RGB(130, 240, 140)
                           : p->on_enemy ? RGB(255, 236, 140)
                                         : RGB(255, 110, 90);
    const uint32_t alpha =
        t < 0.7f ? 256u : (uint32_t)(256.0f * (1.0f - t) / 0.3f);
    text_centered(canvas, true, value, px, (int32_t)py, color, alpha);
  }
}

void h2_xianxia_rpg_render_scene(canvas_t* canvas, const game_t* game) {
  unclip(canvas);
  switch (game->phase) {
    case H2_XIANXIA_RPG_PHASE_TITLE:
    case H2_XIANXIA_RPG_PHASE_ENDING:
    case H2_XIANXIA_RPG_PHASE_PROLOGUE:
      draw_particles(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_CHAPTERS:
      break;
    case H2_XIANXIA_RPG_PHASE_EXPLORE:
      explore_scene(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_BATTLE:
      battle_scene(canvas, game);
      break;
  }
}
