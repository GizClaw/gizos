#include "h2_fruit_slice_font.h"
#include "h2_fruit_slice_cards_render.h"
#include "h2_fruit_slice_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
      exit(1);                                                \
    }                                                         \
  } while (0)

static uint16_t pixels[256 * 64];

static void test_coverage(void) {
  size_t glyphs = 0;
  CHECK(strstr(h2_fruit_slice_font_asset.license_notice,
               "SIL OPEN FONT LICENSE Version 1.1") != NULL);
  CHECK(strstr(h2_fruit_slice_font_asset.license_notice, "2014-2021 Adobe") !=
        NULL);
  for (size_t i = 0; i < h2_fruit_slice_translation_count; ++i) {
    const h2_fruit_slice_translation_t* t = &h2_fruit_slice_translations[i];
    CHECK(strcmp(h2_fruit_slice_translate(H2_FRUIT_SLICE_LANGUAGE_EN, t->en),
                 t->en) == 0);
    CHECK(strcmp(h2_fruit_slice_translate(H2_FRUIT_SLICE_LANGUAGE_ZH_CN, t->en),
                 t->zh) == 0);
    for (size_t j = 0; j < i; ++j)
      CHECK(strcmp(t->en, h2_fruit_slice_translations[j].en) != 0);
    const char* en_format = t->en;
    const char* zh_format = t->zh;
    for (;;) {
      en_format = strchr(en_format, '%');
      zh_format = strchr(zh_format, '%');
      CHECK((en_format == NULL) == (zh_format == NULL));
      if (en_format == NULL) break;
      CHECK(en_format[1] == zh_format[1]);
      en_format += 2;
      zh_format += 2;
    }
    const char* s = t->zh;
    for (uint32_t cp; (cp = h2_fruit_slice_utf8_next(&s)) != 0u;) {
      CHECK(cp != 0xFFFDu);
      if (cp < 128u) continue;
      for (int scale = 1; scale <= 4; ++scale) {
        const h2_fruit_slice_font_strike_t* strike =
            h2_fruit_slice_font_strike(scale);
        const lv_font_fmt_txt_glyph_dsc_t* g =
            h2_fruit_slice_font_glyph(strike, cp);
        CHECK(g != NULL && g->box_w != 0u && g->box_h != 0u);
        /* Horizontal bearings may be moved inside the cell by one pixel.
         * Baselines are shared, so vertical overshoots must already fit. */
        CHECK(g->box_w <= strike->width && g->ofs_x >= -1 &&
              g->ofs_x + g->box_w <= strike->width + 1);
        CHECK(strike->baseline - g->box_h - g->ofs_y >= 0);
        CHECK(strike->baseline - g->ofs_y <= strike->height);
      }
      ++glyphs;
    }
  }
  CHECK(strcmp(h2_fruit_slice_translate(H2_FRUIT_SLICE_LANGUAGE_ZH_CN, "SCORE"),
               "得分") == 0);
  const h2_fruit_slice_font_strike_t* compact = h2_fruit_slice_font_strike(1);
  CHECK(h2_fruit_slice_font_glyph(compact, 0x751Fu) != NULL); /* 生 */
  CHECK(h2_fruit_slice_font_glyph(compact, 0x547Du) != NULL); /* 命 */
  CHECK(h2_fruit_slice_font_glyph(compact, 0x6216u) != NULL); /* 或 */
  CHECK(glyphs > 300u);
  CHECK(h2_fruit_slice_font_glyph(compact, 0x1F600u) == NULL);
  printf("validated %zu translated glyph occurrences across %zu strings\n",
         glyphs, h2_fruit_slice_translation_count);
}

static void test_utf8(void) {
  const char* s = "中A文";
  CHECK(h2_fruit_slice_utf8_next(&s) == 0x4E2Du);
  CHECK(h2_fruit_slice_utf8_next(&s) == 'A');
  CHECK(h2_fruit_slice_utf8_next(&s) == 0x6587u);
  CHECK(h2_fruit_slice_utf8_next(&s) == 0u);
  const char* bad[] = {"\xE4",         "\xE4\xB8",         "\xC0\xAF",
                       "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    s = bad[i];
    const char* before = s;
    CHECK(h2_fruit_slice_utf8_next(&s) == 0xFFFDu && s > before);
    for (unsigned n = 0; *s != '\0' && n < 8u; ++n)
      (void)h2_fruit_slice_utf8_next(&s);
    CHECK(*s == '\0');
  }
}

static void test_measure_and_draw(void) {
  const char* values[] = {"切水果", "金币 999999", "翡翠太刀", "\xE4", "AB"};
  for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v) {
    for (int scale = 1; scale <= 5; ++scale) {
      memset(pixels, 0, sizeof(pixels));
      h2_fruit_slice_canvas_t c = {
          .pixels = pixels, .width = 256, .height = 64, .stride_pixels = 256};
      const int width = h2_fruit_slice_text_width(values[v], scale);
      const int height = h2_fruit_slice_text_height(values[v], scale);
      h2_fruit_slice_draw_text(&c, values[v], 8, 8, scale, 0xFFFFu, 256u);
      unsigned changed = 0;
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 256; ++x) {
          if (pixels[y * 256 + x] == 0) continue;
          CHECK(x >= 8 && x <= 8 + width + scale / 2 + 1);
          CHECK(y >= 8 && y <= 8 + height + scale / 2 + 1);
          ++changed;
        }
      CHECK(changed != 0u);
    }
  }
  CHECK(h2_fruit_slice_text_width("中A文", 1) == 33);
  CHECK(h2_fruit_slice_text_width("中A文", 2) == 46);
}

static void test_native_coverage_and_clipping(void) {
  const uint16_t paper = 0xFFFFu, ink = 0;
  for (int scale = 1; scale <= 4; ++scale) {
    const h2_fruit_slice_font_strike_t* strike =
        h2_fruit_slice_font_strike(scale);
    const lv_font_fmt_txt_glyph_dsc_t* g =
        h2_fruit_slice_font_glyph(strike, 0x7FE1u);
    CHECK(g != NULL); /* 翡 */
    const lv_font_fmt_txt_dsc_t* dsc = strike->font->dsc;
    CHECK(dsc->bitmap_format == 0u && dsc->bpp == (scale <= 2 ? 4u : 2u));
    const unsigned per_byte = 8u / dsc->bpp;
    const unsigned maximum = (1u << dsc->bpp) - 1u;
    const int left = g->ofs_x < 0 ? 0 : g->ofs_x;
    const int top =
        (h2_fruit_slice_text_height("翡", scale) - strike->height) / 2 +
        strike->baseline - g->box_h - g->ofs_y;
    h2_fruit_slice_canvas_t c = {
        .pixels = pixels, .width = 48, .height = 48, .stride_pixels = 256};
    memset(pixels, 0xFF, sizeof(pixels));
    /* Crop the top and left of a dense native glyph. The board path must
     * select exactly those source pixels with neither shadow nor resampling. */
    const int x = -left - 1, y = -top - 2;
    h2_fruit_slice_draw_text(&c, "翡", x, y, scale, ink, 256u);
    unsigned solid = 0, edges = 0;
    for (int py = 0; py < c.height; ++py) {
      for (int px = 0; px < c.width; ++px) {
        uint16_t expected = paper;
        if (py + 2 < g->box_h && px + 1 < g->box_w) {
          const unsigned i = (unsigned)((py + 2) * g->box_w + px + 1);
          const unsigned shift = 8u - dsc->bpp * (1u + i % per_byte);
          const unsigned a =
              (dsc->glyph_bitmap[g->bitmap_index + i / per_byte] >> shift) &
              maximum;
          solid += a == maximum;
          edges += a > 0u && a < maximum;
          expected = h2_fruit_slice_blend(paper, ink,
                                          (a * 256u + maximum / 2u) / maximum);
        }
        CHECK(pixels[py * 256 + px] == expected);
      }
    }
    CHECK(solid != 0u && edges != 0u);
  }
}

static void test_language_touch(void) {
  const int sizes[][2] = {{160, 160}, {240, 240}, {480, 320}, {480, 800}};
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    h2_fruit_slice_game_t game;
    h2_fruit_slice_game_init(&game, sizes[i][0], sizes[i][1], 21u, 0u);
    CHECK(game.language == H2_FRUIT_SLICE_LANGUAGE_ZH_CN);
    h2_fruit_slice_rect_t r;
    CHECK(h2_fruit_slice_game_button_rect(&game, H2_FRUIT_SLICE_BUTTON_LANGUAGE,
                                          &r));
    CHECK(r.x0 >= 0 && r.y0 >= 0 && r.x1 < game.width && r.y1 < game.height);
    const float x = (r.x0 + r.x1) * 0.5f, y = (r.y0 + r.y1) * 0.5f;
    h2_fruit_slice_static_key_t before, after;
    h2_fruit_slice_static_key(&game, &before);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_CANCEL, x, y);
    CHECK(game.language == H2_FRUIT_SLICE_LANGUAGE_ZH_CN);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, x, y);
    CHECK(game.language == H2_FRUIT_SLICE_LANGUAGE_EN);
    h2_fruit_slice_static_key(&game, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) != 0);
    CHECK(game.phase == H2_FRUIT_SLICE_PHASE_TITLE &&
          game.progress.coins == 0u);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
    h2_fruit_slice_game_pointer(&game, H2_FRUIT_SLICE_POINTER_UP, x, y);
    CHECK(game.language == H2_FRUIT_SLICE_LANGUAGE_ZH_CN);
  }
}

static void test_album_text_fits_above_back(void) {
  const int sizes[][2] = {{160, 160}, {240, 240}, {480, 320}};
  for (unsigned size = 0; size < 3u; ++size) {
    h2_fruit_slice_game_t g;
    h2_fruit_slice_game_init(&g, sizes[size][0], sizes[size][1], 1u, 0u);
    h2_fruit_slice_album_layout_t l;
    h2_fruit_slice_cards_album_layout(g.width, g.height, g.unit, &l);
    h2_fruit_slice_rect_t back;
    h2_fruit_slice_game_back_rect(g.width, g.height, &back);
    const float width = (l.detail.x1 - l.detail.x0) * 0.92f;
    CHECK(l.detail.y1 < back.y0);
    for (unsigned language = 0; language < 2u; ++language)
      for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id) {
        const h2_fruit_slice_card_t* card = h2_fruit_slice_card(id);
        const char* name = h2_fruit_slice_translate(
            (h2_fruit_slice_language_t)language, card->name);
        const char* hint = h2_fruit_slice_translate(
            (h2_fruit_slice_language_t)language, card->hint);
        char copies[128];
        (void)snprintf(copies, sizeof(copies), "%s x99", name);
        CHECK((float)h2_fruit_slice_text_width(copies, 1) <= width);
        CHECK((float)h2_fruit_slice_text_width(hint, 1) <= width);
        CHECK((float)(h2_fruit_slice_text_height(copies, 1) +
                      h2_fruit_slice_text_height(hint, 1) + 3) <=
              l.detail.y1 - l.detail.y0);
      }
  }
}

int main(void) {
  test_album_text_fits_above_back();
  test_coverage();
  test_utf8();
  test_measure_and_draw();
  test_native_coverage_and_clipping();
  test_language_touch();
  puts("fruit slice text tests passed");
  return 0;
}
