#include "h2_g99_brand.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

/* Values from the Loader design's layout() at 480 x 480. */
static void test_square_480(void) {
  h2_g99_brand_layout_t l;
  CHECK(h2_g99_brand_layout(480, 480, &l));
  CHECK(l.cell == 5);
  CHECK(l.shadow == 2);
  CHECK(l.icon_w == 60);
  CHECK(l.icon_h == 60);
  CHECK(l.icon_image_w == 62);
  CHECK(l.icon_image_h == 62);
  CHECK(l.gap == 15);
  CHECK(l.group_y == 184);
  CHECK(l.icon_x == 210);
  CHECK(l.wordmark_x == 135);
  CHECK(l.wordmark_y == 259);
  CHECK(l.wordmark_image_w == 212);
  CHECK(l.wordmark_image_h == 37);
  CHECK(l.status_x == 24);
  CHECK(l.status_font == 22);
  CHECK(l.status_baseline == 439);
  CHECK(l.status_top == 422);
  CHECK(l.bar_y == 460);
  CHECK(l.bar_h == 20);
}

/* And at 320 x 240. */
static void test_landscape_320(void) {
  h2_g99_brand_layout_t l;
  CHECK(h2_g99_brand_layout(320, 240, &l));
  CHECK(l.cell == 2);
  CHECK(l.shadow == 1);
  CHECK(l.icon_w == 24);
  CHECK(l.icon_h == 24);
  CHECK(l.icon_image_w == 25);
  CHECK(l.icon_image_h == 25);
  CHECK(l.group_y == 98);
  CHECK(l.icon_x == 148);
  CHECK(l.wordmark_x == 118);
  CHECK(l.wordmark_y == 128);
  CHECK(l.status_x == 12);
  CHECK(l.status_font == 12);
  CHECK(l.status_baseline == 219);
  CHECK(l.status_top == 210);
  CHECK(l.bar_y == 230);
  CHECK(l.bar_h == 10);
}

/* The brand stays on screen and above the status line at other sizes. */
static void test_fits(void) {
  const int32_t sizes[][2] = {
      {480, 480}, {466, 466}, {320, 240}, {240, 320}, {240, 240}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    h2_g99_brand_layout_t l;
    CHECK(h2_g99_brand_layout(sizes[i][0], sizes[i][1], &l));
    CHECK(l.group_y >= 0);
    CHECK(l.wordmark_x + l.wordmark_image_w <= l.width);
    CHECK(l.wordmark_y + l.wordmark_image_h + l.status_x <= l.status_top);
    CHECK(l.status_baseline < l.bar_y);
    CHECK(l.bar_y + l.bar_h == l.height);
  }
}

static void test_rejects_invalid(void) {
  h2_g99_brand_layout_t l;
  CHECK(!h2_g99_brand_layout(0, 240, &l));
  CHECK(!h2_g99_brand_layout(240, 240, NULL));
  CHECK(!h2_g99_brand_layout(40, 40, &l));
}

int main(void) {
  test_square_480();
  test_landscape_320();
  test_fits();
  test_rejects_invalid();
  if (failures != 0) return 1;
  (void)printf("PASS\n");
  return 0;
}
