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
      const lv_font_fmt_txt_glyph_dsc_t* g = h2_fruit_slice_font_glyph(cp);
      CHECK(g != NULL && g->box_w != 0u && g->box_h != 0u);
      /* No Chinese stroke can fall outside the normalized em cell. */
      CHECK(13 - g->box_h - g->ofs_y >= 0);
      CHECK(13 - g->ofs_y <= 15);
      CHECK(g->ofs_x >= 0 && g->ofs_x + g->box_w <= 15);
      ++glyphs;
    }
  }
  CHECK(glyphs > 300u);
  CHECK(h2_fruit_slice_font_glyph(0x1F600u) == NULL);
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
    for (int scale = 1; scale <= 3; ++scale) {
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
  CHECK(h2_fruit_slice_text_width("中A文", 1) == 31);
  CHECK(h2_fruit_slice_text_width("中A文", 2) == 42);
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
  }
}

int main(void) {
  test_coverage();
  test_utf8();
  test_measure_and_draw();
  test_language_touch();
  puts("fruit slice text tests passed");
  return 0;
}
