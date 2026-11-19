#include "buffet_draw.h"
#include "buffet_font_format.h"
#include <math.h>
#include <stdlib.h>

extern const lv_font_t bf_zh_small, bf_zh_body, bf_zh_head, bf_zh_title;
extern const lv_font_t bf_en_small, bf_en_body, bf_en_head, bf_en_title;
static const lv_font_t* const strikes[2][4] = {
    {&bf_zh_small, &bf_zh_body, &bf_zh_head, &bf_zh_title},
    {&bf_en_small, &bf_en_body, &bf_en_head, &bf_en_title}};
enum {
  D = H2_BUFFET_DENSITY,
  W = H2_BUFFET_WIDTH * D,
  H = H2_BUFFET_HEIGHT * D
};
static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }
static uint16_t blend(uint16_t bg, uint16_t fg, unsigned a) {
  if (a == 255) return fg;
  unsigned inv = 255 - a, r = ((bg >> 11) * inv + (fg >> 11) * a + 127) / 255,
           g = (((bg >> 5) & 63) * inv + ((fg >> 5) & 63) * a + 127) / 255,
           b = ((bg & 31) * inv + (fg & 31) * a + 127) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
void bf_box(bf_canvas_t c, int x, int y, int w, int h, uint16_t color) {
  for (int row = max_i(0, y * D); row < min_i(H, (y + h) * D); row++)
    for (int col = max_i(0, x * D); col < min_i(W, (x + w) * D); col++)
      c.pixels[row * W + col] = color;
}
void bf_ellipse(bf_canvas_t c, int x, int y, int rx, int ry, uint16_t color,
                unsigned a) {
  if (rx <= 0 || ry <= 0) return;
  x *= D;
  y *= D;
  rx *= D;
  ry *= D;
  for (int j = max_i(-ry, -y); j <= min_i(ry, H - 1 - y); j++) {
    int w = (int)(rx * sqrtf(fmaxf(0, 1 - (float)(j * j) / (float)(ry * ry))));
    for (int i = max_i(-w, -x); i <= min_i(w, W - 1 - x); i++) {
      uint16_t* p = c.pixels + (y + j) * W + x + i;
      *p = blend(*p, color, a);
    }
  }
}
void bf_line(bf_canvas_t c, int x, int y, int x2, int y2, uint16_t color) {
  x *= D;
  y *= D;
  x2 *= D;
  y2 *= D;
  int dx = abs(x2 - x), dy = -abs(y2 - y), sx = x < x2 ? 1 : -1,
      sy = y < y2 ? 1 : -1, error = dx + dy;
  for (;;) {
    if (x >= 0 && y >= 0 && x < W && y < H) c.pixels[y * W + x] = color;
    if (x == x2 && y == y2) break;
    int e = error * 2;
    if (e >= dy) {
      error += dy;
      x += sx;
    }
    if (e <= dx) {
      error += dx;
      y += sy;
    }
  }
}
void bf_blit(bf_canvas_t c, const bf_sprite_t* s, bf_rect_t src,
             bf_rect_t dst) {
  if (dst.w <= 0 || dst.h <= 0) return;
  dst = (bf_rect_t){dst.x * D, dst.y * D, dst.w * D, dst.h * D};
  for (int y = max_i(0, -dst.y); y < min_i(dst.h, H - dst.y); y++) {
    const uint8_t* row = s->pixels + (src.y + y * src.h / dst.h) * s->w + src.x;
    uint16_t* fb = c.pixels + (dst.y + y) * W;
    for (int x = max_i(0, -dst.x); x < min_i(dst.w, W - dst.x); x++) {
      unsigned index = row[src.w == dst.w ? x : x * src.w / dst.w],
               a = s->alpha[index];
      if (a) fb[dst.x + x] = blend(fb[dst.x + x], s->color[index], a);
    }
  }
}
/* Painted layers keep their native RGB565 pigment and per-pixel coverage.
 * Source images are area filtered to their working size during authoring. */
void bf_layer(bf_canvas_t c, const bf_paint_t* layer, bf_rect_t dst,
              bool mirrored) {
  if (dst.w <= 0 || dst.h <= 0) return;
  dst = (bf_rect_t){dst.x * D, dst.y * D, dst.w * D, dst.h * D};
  for (int y = max_i(0, -dst.y); y < min_i(dst.h, H - dst.y); y++) {
    int source_row = (dst.h == layer->h ? y : y * layer->h / dst.h) * layer->w;
    uint16_t* row = c.pixels + (dst.y + y) * W;
    for (int x = max_i(0, -dst.x); x < min_i(dst.w, W - dst.x); x++) {
      int sx = dst.w == layer->w ? x : x * layer->w / dst.w;
      int i = source_row + (mirrored ? layer->w - 1 - sx : sx);
      unsigned alpha = layer->alpha[i];
      if (alpha) row[dst.x + x] = blend(row[dst.x + x], layer->rgb[i], alpha);
    }
  }
}

/* The registered UV runs visit the steel ribbon in display coordinates.
 * All 64 links revolve with the same fractional lap as the plate slots. */
void bf_steel_track(bf_canvas_t c, int elapsed_ms) {
  unsigned phase =
      (unsigned)(((uint64_t)elapsed_ms * 64 * 128 + bf_rules.lap_ms / 2) /
                 bf_rules.lap_ms);
  const uint16_t* run = bf_belt_runs;
  for (unsigned i = 0; i < bf_belt_run_count; i++) {
    int y = *run++, x = *run++, length = *run++;
    uint16_t* dst = c.pixels + (y + 168 * D) * W + x + 136 * D;
    for (int p = 0; p < length; p++) {
      unsigned uv = *run++;
      unsigned texel = (uv & 0xf80u) | ((uv - phase) & 127u);
      unsigned alpha = (bf_steel.alpha[texel] * (uv >> 12) + 7u) / 15u;
      if (alpha) dst[p] = blend(dst[p], bf_steel.rgb[texel], alpha);
    }
  }
}

void bf_image(bf_canvas_t c, const bf_sprite_t* s, int x, int y, int w, int h) {
  bf_blit(c, s, (bf_rect_t){0, 0, s->w, s->h}, (bf_rect_t){x, y, w, h});
}
/* Strings here are App-owned UTF-8. Missing glyphs are visible and covered by
 * the font test, including the other language's switch label. */
static uint32_t codepoint(const char** next) {
  const unsigned char* s = (const unsigned char*)*next;
  unsigned ch = *s++, count = 0;
  if (ch >= 0xf0) {
    ch &= 7;
    count = 3;
  } else if (ch >= 0xe0) {
    ch &= 15;
    count = 2;
  } else if (ch >= 0xc0) {
    ch &= 31;
    count = 1;
  }
  for (unsigned i = 0; i < count; i++) {
    if ((*s & 0xc0) != 0x80) break;
    ch = ch << 6 | (*s++ & 63);
  }
  *next = (const char*)s;
  return ch;
}
static unsigned glyph(const lv_font_t* f, uint32_t cp) {
  const lv_font_fmt_txt_dsc_t* d = f->dsc;
  for (unsigned m = 0; m < d->cmap_num; m++) {
    const lv_font_fmt_txt_cmap_t* map = &d->cmaps[m];
    if (cp < map->range_start) continue;
    uint32_t off = cp - map->range_start;
    if (off >= map->range_length) continue;
    if (map->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY)
      return map->glyph_id_start + off;
    if (map->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL)
      return map->glyph_id_start +
             ((const uint8_t*)map->glyph_id_ofs_list)[off];
    unsigned lo = 0, hi = map->list_length;
    while (lo < hi) {
      unsigned mid = lo + (hi - lo) / 2;
      if (map->unicode_list[mid] < off)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < map->list_length && map->unicode_list[lo] == off)
      return map->glyph_id_start +
             (map->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
                  ? lo
                  : ((const uint16_t*)map->glyph_id_ofs_list)[lo]);
  }
  return 0;
}
static unsigned lookup(bool en, bf_size_t size, uint32_t cp,
                       const lv_font_t** font) {
  *font = strikes[en][size];
  unsigned id = glyph(*font, cp);
  if (!id) {
    *font = strikes[!en][size];
    id = glyph(*font, cp);
  }
  return id;
}
bool bf_font_covers(bool en, bf_size_t size, const char* s) {
  while (*s) {
    const lv_font_t* font;
    if (!lookup(en, size, codepoint(&s), &font)) return false;
  }
  return true;
}
int bf_text(bf_canvas_t c, bf_size_t size, int x, int baseline, uint16_t color,
            const char* s) {
  int pen = x * D;
  baseline *= D;
  while (*s) {
    const lv_font_t* font;
    unsigned id = lookup(c.en, size, codepoint(&s), &font);
    const lv_font_fmt_txt_dsc_t* d = font->dsc;
    const lv_font_fmt_txt_glyph_dsc_t* g = &d->glyph_dsc[id];
    if (!id) {
      if (c.pixels) bf_box(c, pen / D, baseline / D - 8, 6, 8, color);
      pen += 8 * D;
      continue;
    }
    if (c.pixels) {
      const uint8_t* bits = d->glyph_bitmap + g->bitmap_index;
      int x0 = pen + g->ofs_x, y0 = baseline - g->ofs_y - g->box_h;
      for (int j = max_i(0, -y0); j < min_i(g->box_h, H - y0); j++)
        for (int i = max_i(0, -x0); i < min_i(g->box_w, W - x0); i++) {
          int n = j * g->box_w + i;
          unsigned coverage = (bits[n / 2] >> (n % 2 ? 0 : 4)) & 15;
          if (coverage) {
            uint16_t* p = c.pixels + (y0 + j) * W + x0 + i;
            *p = blend(*p, color, coverage * 17);
          }
        }
    }
    pen += (g->adv_w + 8) >> 4;
  }
  return (pen + D / 2) / D - x;
}
int bf_width(bool en, bf_size_t size, const char* s) {
  return bf_text((bf_canvas_t){NULL, en}, size, 0, 0, 0, s);
}
void bf_center(bf_canvas_t c, bf_size_t size, int x, int y, uint16_t color,
               const char* s) {
  bf_text(c, size, x - bf_width(c.en, size, s) / 2, y, color, s);
}
