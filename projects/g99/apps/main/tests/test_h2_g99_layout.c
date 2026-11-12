#include "h2_g99_layout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

#define NEAR(a, b, eps) CHECK(fabsf((float)(a) - (float)(b)) <= (eps))

static void test_square_480(void) {
  h2_g99_layout_t l;
  CHECK(h2_g99_layout_compute(480, 480, &l));
  NEAR(l.k, 2.0f, 1e-6f);
  CHECK(l.pad == 20);
  CHECK(l.sel_x == 144);
  CHECK(l.cy == 240);
  CHECK(l.radius == 278);
  CHECK(l.cx == 144 - 278);
  CHECK(l.panel_x == 204);
  CHECK(l.panel_w == 256);
  CHECK(l.panel_y == 20);
  CHECK(l.panel_h == 440);
  CHECK(l.font_hero == 36);
  CHECK(l.font_title == 26);
  CHECK(l.font_body == 18);
  CHECK(l.font_small == 15);
  CHECK(l.row_h == 48);
  /* The boot brand is the Loader's (values in libs/art brand_test). */
  h2_g99_brand_layout_t brand;
  CHECK(h2_g99_brand_layout(480, 480, &brand));
  CHECK(memcmp(&l.brand, &brand, sizeof(brand)) == 0);
  CHECK(l.brand.group_y == 184);
  CHECK(l.brand.wordmark_y == 259);
}

static void test_landscape_320(void) {
  h2_g99_layout_t l;
  CHECK(h2_g99_layout_compute(320, 240, &l));
  NEAR(l.k, 1.0f, 1e-6f);
  CHECK(l.pad == 10);
  CHECK(l.sel_x == 96);
  CHECK(l.cy == 120);
  CHECK(l.radius == 139);
  CHECK(l.panel_x == 126);
  CHECK(l.panel_w == 184);
  CHECK(l.font_hero == 22);
  CHECK(l.font_title == 14);
  CHECK(l.font_body == 11);
  CHECK(l.font_small == 10);
  CHECK(l.row_h == 24);
  h2_g99_brand_layout_t brand;
  CHECK(h2_g99_brand_layout(320, 240, &brand));
  CHECK(memcmp(&l.brand, &brand, sizeof(brand)) == 0);
  CHECK(l.brand.group_y == 98);
  CHECK(l.brand.wordmark_x == 118);
}

static void test_rejects_invalid(void) {
  h2_g99_layout_t l;
  CHECK(!h2_g99_layout_compute(0, 480, &l));
  CHECK(!h2_g99_layout_compute(480, -1, &l));
  CHECK(!h2_g99_layout_compute(480, 480, NULL));
}

static void check_wheel(int32_t w, int32_t h, h2_g99_wheel_kind_t kind) {
  h2_g99_layout_t l;
  CHECK(h2_g99_layout_compute(w, h, &l));
  const float step = h2_g99_wheel_step(&l, kind);
  const float deg = step * 180.0f / 3.14159265f;
  CHECK(deg > 18.0f && deg < 26.0f);
  h2_g99_slot_t s;
  h2_g99_wheel_slot(&l, kind, 0.0f, &s);
  NEAR(s.x, l.sel_x, 0.5f);
  NEAR(s.y, l.cy, 0.5f);
  NEAR(s.size, h2_g99_slot_base_size(kind, 0) * l.k, 0.01f);
  NEAR(s.opacity, 1.0f, 1e-6f);
  NEAR(s.glow, 1.0f, 1e-6f);
  NEAR(s.tilt_deg, 0.0f, 1e-4f);
  /* The selection never reaches the info area. */
  CHECK(s.x + s.size / 2.0f < (float)l.panel_x);
  /* The outermost slot touches the vertical padding. */
  h2_g99_wheel_slot(&l, kind, 2.0f, &s);
  NEAR(s.y + s.size / 2.0f, (float)(l.height - l.pad), 1.0f);
  NEAR(s.opacity, 0.22f, 1e-4f);
  NEAR(s.tilt_deg, deg * 2.0f * 0.6f, 0.01f);
  h2_g99_wheel_slot(&l, kind, -2.0f, &s);
  NEAR(s.y - s.size / 2.0f, (float)l.pad, 1.0f);
  CHECK(s.tilt_deg < 0.0f);
  for (float d = -2.0f; d <= 2.0f; d += 0.25f) {
    h2_g99_wheel_slot(&l, kind, d, &s);
    CHECK(s.y - s.size / 2.0f >= (float)l.pad - 1.0f);
    CHECK(s.y + s.size / 2.0f <= (float)(l.height - l.pad) + 1.0f);
    CHECK(s.x - s.size / 2.0f >= -1.0f);
    CHECK(s.x + s.size / 2.0f < (float)l.panel_x);
  }
  h2_g99_wheel_slot(&l, kind, 3.0f, &s);
  CHECK(s.size == 0.0f && s.opacity == 0.0f);
}

static void test_wheel_tiers(void) {
  h2_g99_layout_t l;
  CHECK(h2_g99_layout_compute(480, 480, &l));
  h2_g99_slot_t s;
  h2_g99_wheel_slot(&l, H2_G99_WHEEL_ICONS, 1.0f, &s);
  NEAR(s.size, 44.0f, 0.01f);
  NEAR(s.opacity, 0.55f, 1e-4f);
  CHECK(s.glow == 0.0f);
  h2_g99_wheel_slot(&l, H2_G99_WHEEL_ICONS, 0.5f, &s);
  NEAR(s.size, 56.0f, 0.01f);
  h2_g99_wheel_slot(&l, H2_G99_WHEEL_THUMBS, 0.0f, &s);
  NEAR(s.size, 100.0f, 0.01f);
  h2_g99_wheel_slot(&l, H2_G99_WHEEL_ICONS, -1.0f, &s);
  NEAR(s.size, 44.0f, 0.01f);
  CHECK(s.y < (float)l.cy);
}

static void test_page_frame(void) {
  h2_g99_layout_t l;
  CHECK(h2_g99_layout_compute(480, 480, &l));
  h2_g99_page_frame_t f;
  h2_g99_page_frame(&l, 0.0f, &f);
  NEAR(f.old_wheel_dx, 0.0f, 1e-4f);
  NEAR(f.old_wheel_opacity, 1.0f, 1e-6f);
  NEAR(f.old_info_opacity, 1.0f, 1e-6f);
  NEAR(f.new_wheel_opacity, 0.0f, 1e-6f);
  NEAR(f.new_info_opacity, 0.0f, 1e-6f);
  NEAR(f.new_wheel_dx, -(144.0f + 60.0f), 1e-3f);
  /* Old info ends at 180 ms, old wheel at 270 ms. */
  h2_g99_page_frame(&l, 180.0f / 600.0f, &f);
  NEAR(f.old_info_opacity, 0.0f, 1e-4f);
  h2_g99_page_frame(&l, 270.0f / 600.0f, &f);
  NEAR(f.old_wheel_opacity, 0.0f, 1e-4f);
  NEAR(f.old_wheel_dx, -204.0f, 1e-3f);
  /* New wheel starts at 210 ms and lands at 480 ms; new info from 360 ms. */
  h2_g99_page_frame(&l, 210.0f / 600.0f, &f);
  NEAR(f.new_wheel_opacity, 0.0f, 1e-4f);
  h2_g99_page_frame(&l, 360.0f / 600.0f, &f);
  NEAR(f.new_info_opacity, 0.0f, 1e-4f);
  CHECK(f.new_wheel_opacity > 0.0f);
  h2_g99_page_frame(&l, 480.0f / 600.0f, &f);
  NEAR(f.new_wheel_opacity, 1.0f, 1e-4f);
  NEAR(f.new_wheel_dx, 0.0f, 1e-3f);
  h2_g99_page_frame(&l, 1.0f, &f);
  NEAR(f.new_info_opacity, 1.0f, 1e-6f);
}

static void test_boot_frame(void) {
  h2_g99_boot_frame_t f;
  h2_g99_boot_frame(0u, &f);
  NEAR(f.brand_opacity, 1.0f, 1e-6f);
  NEAR(f.background_opacity, 0.0f, 1e-6f);
  NEAR(f.unfold, 0.0f, 1e-6f);
  NEAR(f.info_opacity, 0.0f, 1e-6f);
  h2_g99_boot_frame(200u, &f);
  NEAR(f.background_opacity, 0.0f, 1e-6f);
  h2_g99_boot_frame(300u, &f);
  NEAR(f.brand_opacity, 0.0f, 1e-6f);
  h2_g99_boot_frame(350u, &f);
  NEAR(f.unfold, 0.0f, 1e-6f);
  h2_g99_boot_frame(600u, &f);
  NEAR(f.background_opacity, 1.0f, 1e-6f);
  NEAR(f.info_opacity, 0.0f, 1e-6f);
  h2_g99_boot_frame(700u, &f);
  NEAR(f.unfold, 1.0f, 1e-6f);
  h2_g99_boot_frame(H2_G99_BOOT_MS, &f);
  NEAR(f.info_opacity, 1.0f, 1e-6f);
  /* The unfolded boot wheel lands exactly on the main wheel. */
  h2_g99_layout_t l;
  CHECK(h2_g99_layout_compute(320, 240, &l));
  for (int d = -2; d <= 2; ++d) {
    h2_g99_slot_t boot;
    h2_g99_slot_t wheel;
    h2_g99_boot_slot(&l, d, 1.0f, &boot);
    h2_g99_wheel_slot(&l, H2_G99_WHEEL_ICONS, (float)d, &wheel);
    NEAR(boot.x, wheel.x, 1e-3f);
    NEAR(boot.y, wheel.y, 1e-3f);
    NEAR(boot.size, wheel.size, 1e-3f);
    NEAR(boot.opacity, wheel.opacity, 1e-4f);
    h2_g99_boot_slot(&l, d, 0.0f, &boot);
    NEAR(boot.y, l.cy, 1e-3f);
    NEAR(boot.opacity, 0.0f, 1e-6f);
  }
}

static void test_spans_and_rows(void) {
  int32_t lo = 0;
  int32_t hi = 0;
  h2_g99_wheel_span(1u, &lo, &hi);
  CHECK(lo == 0 && hi == 0);
  h2_g99_wheel_span(4u, &lo, &hi);
  CHECK(lo == -1 && hi == 2);
  h2_g99_wheel_span(5u, &lo, &hi);
  CHECK(lo == -2 && hi == 2);
  h2_g99_wheel_span(13u, &lo, &hi);
  CHECK(lo == -2 && hi == 2);
  h2_g99_layout_t l;
  CHECK(h2_g99_layout_compute(480, 480, &l));
  CHECK(h2_g99_layout_rows(&l, 100, 100 + 48 * 6 + 47) == 6);
  CHECK(h2_g99_layout_rows(&l, 100, 90) == 1);
}

static void test_other_resolutions(void) {
  const int32_t sizes[][2] = {{480, 480}, {320, 240}, {368, 448}, {240, 240}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    h2_g99_layout_t l;
    CHECK(h2_g99_layout_compute(sizes[i][0], sizes[i][1], &l));
    CHECK(l.panel_w > 0 && l.panel_h > 0);
    CHECK(l.panel_x + l.panel_w + l.pad == l.width);
    CHECK(l.brand.wordmark_x + l.brand.wordmark_image_w <= l.width);
    CHECK(l.brand.group_y >= 0);
    CHECK(l.brand.wordmark_y + l.brand.wordmark_image_h <= l.height);
    check_wheel(sizes[i][0], sizes[i][1], H2_G99_WHEEL_ICONS);
    check_wheel(sizes[i][0], sizes[i][1], H2_G99_WHEEL_THUMBS);
  }
}

static void test_wheel_bounds(void) {
  const int32_t sizes[][2] = {{480, 480}, {320, 240}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    h2_g99_layout_t l;
    CHECK(h2_g99_layout_compute(sizes[i][0], sizes[i][1], &l));
    h2_g99_rect_t r;
    h2_g99_wheel_bounds(&l, H2_G99_WHEEL_ICONS, 5u, &r);
    /* The five-slot wheel stays on screen and left of the info area. */
    CHECK(r.x1 >= 0 && r.y1 >= 0 && r.x2 <= l.width && r.y2 <= l.height);
    CHECK(r.x2 <= l.panel_x);
    CHECK(r.y1 <= l.pad && r.y2 >= l.height - l.pad);
    h2_g99_rect_t single;
    h2_g99_wheel_bounds(&l, H2_G99_WHEEL_THUMBS, 1u, &single);
    const int32_t thumb = (int32_t)(50.0f * l.k);
    CHECK(single.x2 - single.x1 >= thumb && single.x2 - single.x1 <= thumb + 2);
    CHECK(single.x2 <= l.panel_x);
    h2_g99_rect_t none = {1, 1, 1, 1};
    h2_g99_wheel_bounds(&l, H2_G99_WHEEL_ICONS, 0u, &none);
    CHECK(none.x1 == 0 && none.x2 == 0 && none.y1 == 0 && none.y2 == 0);
  }
}

/* Large pointer jumps and a delayed frame remain bounded; reversing and
 * returning to the origin converge without overshoot. */
static void test_wheel_motion(void) {
  float offset = h2_g99_wheel_follow(0.0f, 1000.0f, 10000u);
  CHECK(offset > 0.0f && offset <= 0.181f);
  CHECK(h2_g99_wheel_follow(offset, 1.0f, 0u) == offset);
  for (int i = 0; i < 100; ++i) {
    const float next = h2_g99_wheel_follow(offset, 1000.0f, 10u);
    CHECK(next >= offset && next <= 1.0f);
    CHECK(next - offset <= 0.037f);
    offset = next;
  }
  NEAR(offset, 1.0f, 0.001f);
  for (int i = 0; i < 100; ++i) {
    const float next = h2_g99_wheel_follow(offset, -1000.0f, 10u);
    CHECK(next <= offset && next >= -1.0f);
    CHECK(offset - next <= 0.037f);
    offset = next;
  }
  NEAR(offset, -1.0f, 0.001f);
  for (int i = 0; i < 100; ++i) offset = h2_g99_wheel_follow(offset, 0.0f, 10u);
  CHECK(offset == 0.0f);
  CHECK(h2_g99_wheel_snap(0.29f) == 0);
  CHECK(h2_g99_wheel_snap(0.5f) == 1);
  CHECK(h2_g99_wheel_snap(-0.5f) == -1);
  CHECK(h2_g99_wheel_snap(1000.0f) == 1);
  CHECK(h2_g99_wheel_snap(-1000.0f) == -1);
  CHECK(h2_g99_ease_snap(-1.0f) == 0.0f);
  CHECK(h2_g99_ease_snap(2.0f) == 1.0f);
  float previous = 0.0f;
  for (int i = 0; i <= 100; ++i) {
    const float value = h2_g99_ease_snap((float)i / 100.0f);
    CHECK(value >= previous && value <= 1.0f);
    previous = value;
  }
  CHECK(previous == 1.0f);
  h2_g99_layout_t layout;
  CHECK(h2_g99_layout_compute(480, 480, &layout));
  for (int i = 0; i <= 100; ++i) {
    const float position = (float)i / 100.0f;
    h2_g99_slot_t before, after;
    h2_g99_wheel_slot(&layout, H2_G99_WHEEL_ICONS, -position, &before);
    h2_g99_wheel_slot(&layout, H2_G99_WHEEL_ICONS, 1.0f - position, &after);
    CHECK(before.glow >= 0.5f || after.glow >= 0.5f);
  }
}

static void test_list_motion(void) {
  h2_g99_list_motion_t motion = {0};
  h2_g99_list_select(&motion, 0u, 4u, 1000u, false);
  h2_g99_list_begin(&motion, 1000u);
  h2_g99_list_drag(&motion, 100.0f, 4u, 6000u);
  CHECK(motion.position > 0.0f && motion.position <= 0.401f);
  float previous = motion.position;
  for (uint64_t moment = 6010u; moment < 7010u; moment += 10u) {
    h2_g99_list_drag(&motion, 100.0f, 4u, moment);
    CHECK(motion.position >= previous && motion.position <= 3.0f);
    CHECK(motion.position - previous <= 0.081f);
    previous = motion.position;
  }
  NEAR(motion.position, 3.0f, 0.001f);
  motion.position = 1.65f;
  CHECK(h2_g99_list_release(&motion, 4u, 8000u) == 2u);
  CHECK(motion.snapping && !motion.dragging);
  h2_g99_list_tick(&motion, 8000u + H2_G99_ROTATE_MS / 2u);
  CHECK(motion.position > 1.65f && motion.position < 2.0f);
  h2_g99_list_tick(&motion, 8000u + H2_G99_ROTATE_MS);
  CHECK(motion.position == 2.0f && !motion.snapping);
  h2_g99_list_select(&motion, 0u, 4u, 9000u, true);
  h2_g99_list_tick(&motion, 9050u);
  h2_g99_list_begin(&motion, 9060u);
  CHECK(motion.position == 0.0f && motion.origin == 0.0f);
  CHECK(!motion.snapping && motion.dragging);
  h2_g99_list_drag(&motion, -100.0f, 4u, 9110u);
  CHECK(motion.position == 0.0f);
  h2_g99_list_select(&motion, 255u, 1u, 9200u, false);
  CHECK(motion.position == 0.0f && motion.target == 0u);
  CHECK(h2_g99_list_release(&motion, 0u, 9300u) == 0u);
  motion.position = NAN;
  CHECK(h2_g99_list_release(&motion, 4u, 9400u) == 0u);
  CHECK(motion.position == 0.0f && !motion.snapping);
}

int main(void) {
  test_list_motion();
  test_wheel_motion();
  test_square_480();
  test_landscape_320();
  test_rejects_invalid();
  test_wheel_tiers();
  test_page_frame();
  test_boot_frame();
  test_spans_and_rows();
  test_other_resolutions();
  test_wheel_bounds();
  if (failures != 0) {
    fprintf(stderr, "%d layout check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
