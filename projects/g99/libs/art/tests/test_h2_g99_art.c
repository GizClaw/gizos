#include "h2_g99_art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static uint8_t alpha_at(const uint8_t* argb, int32_t size, float gx, float gy) {
  /* Grid coordinates on the 24-unit design grid. */
  const int32_t x = (int32_t)(gx * (float)size / 24.0f);
  const int32_t y = (int32_t)(gy * (float)size / 24.0f);
  return argb[((size_t)y * (size_t)size + (size_t)x) * 4u + 3u];
}

static void test_icons(void) {
  enum { kSize = 96 };
  static uint8_t argb[kSize * kSize * 4];
  for (int i = 0; i < H2_G99_ICON_COUNT; ++i) {
    CHECK(h2_g99_art_icon((h2_g99_icon_t)i, 0xFFFFFFu, argb, kSize));
    size_t covered = 0u;
    for (size_t p = 0; p < (size_t)kSize * kSize; ++p)
      if (argb[p * 4u + 3u] != 0u) ++covered;
    /* Every icon is a visible flat shape that leaves transparent space. */
    CHECK(covered > (size_t)kSize * kSize / 20u);
    CHECK(covered < (size_t)kSize * kSize * 9u / 10u);
  }
  CHECK(h2_g99_art_icon(H2_G99_ICON_LIBRARY, 0x9B5CFFu, argb, kSize));
  CHECK(alpha_at(argb, kSize, 7.0f, 7.0f) == 255u);
  CHECK(alpha_at(argb, kSize, 12.0f, 12.0f) == 0u);
  const uint8_t* p = argb + ((size_t)28 * kSize + 28) * 4u;
  CHECK(p[0] == 0xFFu && p[1] == 0x5Cu && p[2] == 0x9Bu && p[3] == 255u);
  CHECK(h2_g99_art_icon(H2_G99_ICON_SETTINGS, 0xFFFFFFu, argb, kSize));
  CHECK(alpha_at(argb, kSize, 12.0f, 12.0f) == 0u);
  CHECK(alpha_at(argb, kSize, 12.0f, 6.0f) == 255u);
  CHECK(h2_g99_art_icon(H2_G99_ICON_INFO, 0xFFFFFFu, argb, kSize));
  CHECK(alpha_at(argb, kSize, 12.0f, 14.0f) == 0u);
  CHECK(alpha_at(argb, kSize, 12.0f, 3.5f) == 255u);
  CHECK(alpha_at(argb, kSize, 0.5f, 0.5f) == 0u);
  CHECK(!h2_g99_art_icon(H2_G99_ICON_COUNT, 0u, argb, kSize));
  CHECK(!h2_g99_art_icon(H2_G99_ICON_ME, 0u, NULL, kSize));
  CHECK(!h2_g99_art_icon(H2_G99_ICON_ME, 0u, argb, 0));
}

static void test_wordmark(void) {
  CHECK(!h2_g99_art_wordmark_cell(0, 0));
  CHECK(h2_g99_art_wordmark_cell(1, 0));
  CHECK(!h2_g99_art_wordmark_cell(5, 3));
  CHECK(h2_g99_art_wordmark_cell(6, 0));
  CHECK(h2_g99_art_wordmark_cell(41, 6));
  CHECK(!h2_g99_art_wordmark_cell(42, 6));
  CHECK(!h2_g99_art_wordmark_cell(0, 7));
  enum {
    kCell = 5,
    kShadow = 2,
    kW = 42 * kCell + kShadow,
    kH = 7 * kCell + kShadow
  };
  static uint8_t argb[kW * kH * 4];
  CHECK(h2_g99_art_wordmark(argb, kCell, kShadow));
  const uint8_t* top = argb + ((size_t)0 * kW + kCell) * 4u;
  CHECK(top[2] == 0x9Bu && top[3] == 255u);
  const uint8_t* shadow =
      argb + ((size_t)(kCell + 1) * kW + 2 * kCell + 1) * 4u;
  CHECK(shadow[3] == 255u);
  CHECK(argb[3] == 0u);
}

static void test_brand_cover_glow(void) {
  enum { kW = 48, kH = 62 };
  static uint8_t brand[kW * kH * 4];
  CHECK(h2_g99_art_brand_icon(brand, kW, kH));
  /* Screen area is the deep colour; the far corner is transparent. */
  const uint8_t* screen = brand + ((size_t)10 * kW + 10) * 4u;
  CHECK(screen[3] == 255u && screen[2] == 0x1Cu);
  CHECK(brand[((size_t)(kH - 1) * kW) * 4u + 3u] == 0u);
  enum { kCover = 100 };
  static uint8_t cover[kCover * kCover * 4];
  CHECK(h2_g99_art_cover(cover, kCover));
  CHECK(cover[3] == 0u);
  CHECK(cover[((size_t)50 * kCover + 50) * 4u + 3u] == 255u);
  enum { kGlow = 64 };
  static uint8_t glow[kGlow * kGlow * 4];
  CHECK(h2_g99_art_glow(glow, kGlow));
  CHECK(glow[((size_t)32 * kGlow + 32) * 4u + 3u] == 255u);
  CHECK(glow[3] == 0u);
}

static void test_background(void) {
  const h2_g99_background_t spec = {320, 240, 102, 1.0f};
  const size_t pixels = 320u * 240u;
  uint16_t* a = malloc(pixels * sizeof(uint16_t));
  uint16_t* b = malloc(pixels * sizeof(uint16_t));
  uint16_t* c = malloc(pixels * sizeof(uint16_t));
  CHECK(a != NULL && b != NULL && c != NULL);
  if (a == NULL || b == NULL || c == NULL) {
    free(a);
    free(b);
    free(c);
    return;
  }
  CHECK(h2_g99_background_render(&spec, 0u, a, 320, 0, 240));
  CHECK(h2_g99_background_render(&spec, 4000u, b, 320, 0, 240));
  const int32_t top = h2_g99_background_wave_top(&spec);
  CHECK(top > 0 && top < 240);
  /* Flow only changes rows at or below the wave top. */
  CHECK(memcmp(a, b, (size_t)top * 320u * sizeof(uint16_t)) == 0);
  CHECK(memcmp(a + (size_t)top * 320u, b + (size_t)top * 320u,
               (pixels - (size_t)top * 320u) * sizeof(uint16_t)) != 0);
  /* Partial renders match the corresponding full-frame rows. */
  memcpy(c, a, pixels * sizeof(uint16_t));
  CHECK(h2_g99_background_render(&spec, 4000u, c, 320, top, 240));
  CHECK(memcmp(c, b, pixels * sizeof(uint16_t)) == 0);
  /* The info shade darkens the right side. */
  CHECK((a[120u * 320u + 300u] >> 11) <= (a[120u * 320u + 60u] >> 11) + 4u);
  CHECK(!h2_g99_background_render(&spec, 0u, a, 100, 0, 240));
  CHECK(!h2_g99_background_render(NULL, 0u, a, 320, 0, 240));
  free(a);
  free(b);
  free(c);
}

int main(void) {
  test_icons();
  test_wordmark();
  test_brand_cover_glow();
  test_background();
  if (failures != 0) {
    fprintf(stderr, "%d art check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
