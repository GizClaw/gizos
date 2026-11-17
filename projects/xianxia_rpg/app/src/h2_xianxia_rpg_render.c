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
static int32_t text_wrapped(canvas_t* canvas, const char* text, int32_t x,
                            int32_t y, int32_t max_w, int32_t max_lines,
                            uint16_t color, uint32_t alpha,
                            uint32_t max_chars) {
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
        (void)glyph(canvas, font, cp, gx + 1, gy + 1, C_SHADOW,
                    alpha * 3u / 4u);
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

static void paper_landscape(canvas_t* canvas, const game_t* game, bool dawn) {
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

static void battle_backdrop(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float w = (float)canvas->width, sh = (float)l->stage.h;
  const float u = game->unit;
  const bool shrine = game->map == H2_XIANXIA_RPG_MAP_SHRINE;
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

static void tile_art(canvas_t* canvas, const game_t* game, int32_t col,
                     int32_t row) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const h2_xianxia_rpg_map_t map = game->map;
  const float t = (float)l->tile;
  const float x = (float)(l->map_x + col * l->tile);
  const float y = (float)(l->map_y + row * l->tile);
  const uint32_t seed = hash32((uint32_t)(map * 977 + row * 31 + col));
  const bool returned = game->stage >= H2_XIANXIA_RPG_STAGE_RETURN;
  const ground_t gr = ground_for(map, returned);
  const char tile = h2_xianxia_rpg_tile(map, col, row);
  const bool indoor = map == H2_XIANXIA_RPG_MAP_SHRINE;
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
      if (map == H2_XIANXIA_RPG_MAP_VILLAGE && returned && seed % 3u == 0u) {
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

static void map_background(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float w = (float)canvas->width, h = (float)canvas->height;
  gradient_rect(canvas, 0.0f, 0.0f, w, h, c3(30, 26, 34), c3(18, 16, 22));
  for (int32_t row = 0; row < ROWS; ++row)
    for (int32_t col = 0; col < COLS; ++col) tile_art(canvas, game, col, row);
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

/* ---- Battle sprites ---------------------------------------------------- */

static void draw_enemy(canvas_t* canvas, h2_xianxia_rpg_enemy_t enemy, float cx,
                       float cy, float s, uint64_t time_ms, bool charging) {
  const h2_xianxia_rpg_enemy_info_t* info = h2_xianxia_rpg_enemy_info(enemy);
  const float phase = (float)(time_ms % 2400u) * (2.0f * PI / 2400.0f);
  const float bob = sinf(phase) * s * 0.04f;
  cy += bob;
  switch (enemy) {
    case H2_XIANXIA_RPG_ENEMY_LEAF_SPRITE:
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
    case H2_XIANXIA_RPG_ENEMY_CIVET: {
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
    case H2_XIANXIA_RPG_ENEMY_GOLEM: {
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
  }
}

void h2_xianxia_rpg_static_key(const game_t* game,
                               h2_xianxia_rpg_layer_key_t* out) {
  memset(out, 0, sizeof(*out));
  int32_t* f = out->fields;
  f[0] = (int32_t)game->phase;
  switch (game->phase) {
    case H2_XIANXIA_RPG_PHASE_TITLE:
      f[1] = game->has_save ? 1 : 0;
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
        f[10] = (int32_t)(intptr_t)(game->dialogue.active
                                        ? game->dialogue.script -
                                              h2_xianxia_rpg_script(
                                                  H2_XIANXIA_RPG_SCRIPT_INTRO)
                                        : -1);
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
      }
      break;
    case H2_XIANXIA_RPG_PHASE_ENDING:
      f[1] = game->time_ms >= game->phase_started_ms + 1500u ? 1 : 0;
      f[2] = (int32_t)game->hero.level;
      break;
  }
}

/* ---- Static layers ----------------------------------------------------- */

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
  /* Small screens keep the chapter name and drop the subtitle. */
  const int32_t sub_y = ty + big->line_height + 10;
  const bool roomy =
      sub_y + 2 * LINE_PX + 10 <= game->layout.title_buttons[0].y;
  if (roomy)
    paper_centered(canvas, false,
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_SUBTITLE], w * 0.5f,
                   sub_y, RGB(80, 70, 70), 240u);
  paper_centered(canvas, false,
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_CHAPTER], w * 0.5f,
                 roomy ? sub_y + LINE_PX + 6 : sub_y, RGB(170, 44, 38), 256u);
  for (int32_t i = 0; i < H2_XIANXIA_RPG_TITLE_BUTTONS; ++i) {
    const h2_xianxia_rpg_rect_t* r = &game->layout.title_buttons[i];
    const bool enabled = i == 0 || game->has_save;
    const float x0 = (float)r->x, y0 = (float)r->y;
    const float x1 = x0 + (float)r->w, y1 = y0 + (float)r->h;
    rounded_rect(canvas, x0 + 3.0f, y0 + 4.0f, x1 + 3.0f, y1 + 4.0f, 10.0f,
                 RGB(60, 50, 40), 70u);
    rounded_rect(canvas, x0, y0, x1, y1, 10.0f,
                 enabled ? C_GOLD : RGB(170, 160, 150), 256u);
    rounded_rect(canvas, x0 + 2.0f, y0 + 2.0f, x1 - 2.0f, y1 - 2.0f, 8.5f,
                 enabled ? RGB(48, 40, 46) : RGB(120, 114, 110), 256u);
    text_centered(canvas, false,
                  h2_xianxia_rpg_text[i == 0 ? H2_XIANXIA_RPG_TEXT_NEW_GAME
                                             : H2_XIANXIA_RPG_TEXT_CONTINUE],
                  (x0 + x1) * 0.5f, (int32_t)((y0 + y1) * 0.5f) - LINE_PX / 2,
                  enabled ? C_TEXT : RGB(200, 194, 186), 256u);
  }
  const h2_xianxia_rpg_rect_t* last =
      &game->layout.title_buttons[H2_XIANXIA_RPG_TITLE_BUTTONS - 1];
  const int32_t hint_y = (int32_t)(h - (float)LINE_PX - 10.0f);
  if (hint_y >= last->y + last->h + 4)
    paper_centered(canvas, false,
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_TITLE_HINT],
                   w * 0.5f, hint_y, RGB(110, 100, 96), 220u);
}

static void draw_ending(canvas_t* canvas, const game_t* game) {
  const float w = (float)canvas->width, h = (float)canvas->height;
  const int32_t big = font_of(true)->line_height;
  const float pw = fminf(w * 0.86f, 520.0f);
  const float ph = (float)(big + 6 * LINE_PX + 48);
  const float px = (w - pw) * 0.5f, py = h * 0.5f - ph * 0.5f;
  rounded_rect(canvas, px, py, px + pw, py + ph, 16.0f, RGB(250, 244, 230),
               200u);
  int32_t y = (int32_t)py + 14;
  paper_centered(canvas, true,
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_ENDING_TITLE],
                 w * 0.5f, y, RGB(170, 40, 36), 256u);
  y += big + 8;
  paper_centered(canvas, false,
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_ENDING_1], w * 0.5f, y,
                 C_INK, 256u);
  y += LINE_PX + 4;
  paper_centered(canvas, false,
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_ENDING_2], w * 0.5f, y,
                 C_INK, 256u);
  y += LINE_PX + 6;
  paper_centered(canvas, false,
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_ENDING_NEXT], w * 0.5f,
                 y, RGB(170, 44, 38), 256u);
  y += LINE_PX + 12;
  char stats[96];
  (void)snprintf(stats, sizeof(stats),
                 h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_ENDING_STATS],
                 (unsigned)game->hero.level, (unsigned)game->battles_won,
                 (unsigned)game->turns_taken);
  paper_centered(canvas, false, stats, w * 0.5f, y, RGB(90, 80, 80), 240u);
  y += LINE_PX + 6;
  if (game->time_ms >= game->phase_started_ms + 1500u)
    paper_centered(canvas, false,
                   h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_ENDING_HINT],
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
  text_wrapped(canvas, line->text, x, text_y, w,
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

static void draw_battle_static(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const h2_xianxia_rpg_battle_t* b = &game->battle;
  const h2_xianxia_rpg_enemy_info_t* info =
      h2_xianxia_rpg_enemy_info((h2_xianxia_rpg_enemy_t)b->enemy);
  const int32_t w = canvas->width;
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
  /* Lower panel: message and commands. */
  fill_rect(canvas, (float)l->panel.x, (float)l->panel.y, (float)l->panel.w,
            (float)l->panel.h, RGB(20, 16, 24), 256u);
  fill_rect(canvas, (float)l->panel.x, (float)l->panel.y, (float)l->panel.w,
            2.0f, C_GOLD, 220u);
  const h2_xianxia_rpg_rect_t* m = &l->message;
  panel(canvas, (float)m->x, (float)m->y, (float)m->w, (float)m->h, 8.0f, 230u);
  text_wrapped(canvas, b->message, m->x + 12, m->y + 8, m->w - 24, 2, C_TEXT,
               256u, UINT32_MAX);
  const bool finished = b->state == H2_XIANXIA_RPG_BATTLE_VICTORY ||
                        b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT;
  if (finished && b->state_ms >= 450u) {
    const char* hint = h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_TAP_NEXT];
    const int32_t hw = h2_xianxia_rpg_text_width(false, hint);
    (void)text_at(canvas, false, hint, m->x + m->w - hw - 12,
                  m->y + m->h - LINE_PX - 4, C_GOLD, 230u);
  }
  const bool choosing = b->state == H2_XIANXIA_RPG_BATTLE_COMMAND;
  for (int32_t i = 0; i < H2_XIANXIA_RPG_COMMANDS; ++i) {
    const h2_xianxia_rpg_rect_t* r = &l->commands[i];
    const bool available =
        h2_xianxia_rpg_command_available(game, (h2_xianxia_rpg_command_t)i);
    const bool lit = choosing && available;
    const float x0 = (float)r->x, y0 = (float)r->y;
    const float x1 = x0 + (float)r->w, y1 = y0 + (float)r->h;
    rounded_rect(canvas, x0, y0, x1, y1, 8.0f, lit ? C_GOLD : RGB(84, 76, 70),
                 256u);
    rounded_rect(canvas, x0 + 2.0f, y0 + 2.0f, x1 - 2.0f, y1 - 2.0f, 6.5f,
                 lit ? RGB(74, 46, 40) : RGB(44, 38, 44), 256u);
    if (lit)
      rounded_rect(canvas, x0 + 3.0f, y0 + 3.0f, x1 - 3.0f,
                   y0 + (float)r->h * 0.45f, 5.0f, RGB(255, 230, 180), 40u);
    text_centered(canvas, false,
                  h2_xianxia_rpg_command_label((h2_xianxia_rpg_command_t)i),
                  (x0 + x1) * 0.5f, (int32_t)((y0 + y1) * 0.5f) - LINE_PX / 2,
                  lit ? C_TEXT : RGB(130, 124, 120), 256u);
  }
}

/* ---- Public renderers -------------------------------------------------- */

void h2_xianxia_rpg_render_background(canvas_t* canvas, const game_t* game) {
  unclip(canvas);
  switch (game->phase) {
    case H2_XIANXIA_RPG_PHASE_TITLE:
      paper_landscape(canvas, game, false);
      break;
    case H2_XIANXIA_RPG_PHASE_ENDING:
      paper_landscape(canvas, game, true);
      break;
    case H2_XIANXIA_RPG_PHASE_EXPLORE:
      map_background(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_BATTLE:
      battle_backdrop(canvas, game);
      break;
  }
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
    case H2_XIANXIA_RPG_PHASE_EXPLORE:
      draw_hud(canvas, game);
      draw_banner(canvas, game);
      draw_dialogue(canvas, game);
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
    default:
      return false;
  }
}

static void explore_scene(canvas_t* canvas, const game_t* game) {
  const h2_xianxia_rpg_layout_t* l = &game->layout;
  const float t = (float)l->tile;
  int32_t clip_bottom = l->map_y + ROWS * l->tile;
  if (!l->stacked && game->dialogue.active && l->dialog.y < clip_bottom)
    clip_bottom = l->dialog.y;
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

  sprite_t sprites[H2_XIANXIA_RPG_MAX_NPCS + 1];
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
    else
      draw_person(canvas, (h2_xianxia_rpg_actor_t)s->actor, s->x, s->feet, t,
                  s->facing, s->walk, s->bob);
    if (s->quest) {
      const float top =
          s->feet -
          t * (s->actor == H2_XIANXIA_RPG_ACTOR_DRAGON ? 1.55f : 1.4f);
      const float lift = sinf(idle * 2.0f) * t * 0.05f;
      glow(canvas, s->x, top + lift, t * 0.45f, RGB(255, 214, 110), 150u);
      diamond(canvas, s->x, top + lift, t * 0.23f, RGB(60, 30, 10), 230u);
      diamond(canvas, s->x, top + lift, t * 0.18f, C_GOLD, 256u);
      diamond(canvas, s->x - t * 0.03f, top + lift - t * 0.06f, t * 0.06f,
              RGB(255, 250, 222), 230u);
    }
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
  const float s = fminf(sh * 0.5f, w * 0.26f);
  clip_to(canvas, 0, 2 * LINE_PX + 20, canvas->width, l->stage.h);
  float ex = w * 0.68f, ey = sh * 0.6f;
  float hx = w * 0.26f;
  const float hfeet = sh * 0.94f;
  const float duration = (float)H2_XIANXIA_RPG_ACT_MS;
  const float k = clampf((float)b->state_ms / (duration * 0.6f), 0.0f, 1.0f);
  if (b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT &&
      (b->command == H2_XIANXIA_RPG_COMMAND_ATTACK ||
       b->command == H2_XIANXIA_RPG_COMMAND_SKILL))
    hx += sinf(k * PI) * (ex - hx) * 0.45f;
  if (b->state == H2_XIANXIA_RPG_BATTLE_ENEMY_ACT && b->hit_count > 0u)
    ex -= sinf(k * PI) * (ex - hx) * 0.3f;
  const float shake = ((game->time_ms / 30u) % 2u ? 1.0f : -1.0f) * 3.0f * u;
  if (b->enemy_flash_ms > 0u) ex += shake;
  if (b->hero_flash_ms > 0u) hx += shake;

  /* Enemy, or its dispersal after defeat. */
  if (b->state != H2_XIANXIA_RPG_BATTLE_VICTORY) {
    draw_enemy(canvas, (h2_xianxia_rpg_enemy_t)b->enemy, ex, ey, s,
               game->time_ms, b->charging);
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

  /* Hero, side view facing the enemy. */
  const bool fallen = b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT;
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

  /* Sword light at each hit. */
  if (b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT) {
    for (uint32_t i = 0; i < b->hit_count; ++i) {
      const int32_t at =
          (int32_t)(H2_XIANXIA_RPG_HIT_MS + i * H2_XIANXIA_RPG_HIT_GAP_MS);
      const int32_t dt = (int32_t)b->state_ms - at;
      if (dt < -60 || dt > 180) continue;
      const float fade = 1.0f - clampf((float)dt / 180.0f, 0.0f, 1.0f);
      const float dir = i % 2u ? -1.0f : 1.0f;
      const uint16_t color = b->command == H2_XIANXIA_RPG_COMMAND_SKILL
                                 ? RGB(140, 240, 255)
                                 : RGB(255, 246, 210);
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
          const float x = ex - (ex - hx + s * 0.4f) * t;
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
    if (!p->active) continue;
    const float t = (float)p->age_ms / (float)H2_XIANXIA_RPG_POPUP_MS;
    char value[16];
    (void)snprintf(value, sizeof(value), p->heal ? "+%u" : "%u",
                   (unsigned)p->value);
    const float px = (p->on_enemy ? ex : hx) + (float)p->slot * s * 0.18f;
    const float py =
        (p->on_enemy ? ey - s * 0.5f : hfeet - s * 1.1f) - t * s * 0.35f;
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
      draw_particles(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_EXPLORE:
      explore_scene(canvas, game);
      break;
    case H2_XIANXIA_RPG_PHASE_BATTLE:
      battle_scene(canvas, game);
      break;
  }
}
