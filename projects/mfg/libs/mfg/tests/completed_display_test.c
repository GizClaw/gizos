#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"
#include "lvgl.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(h2_mfg_factory_font);

static unsigned opened, presented, closed;
static int width = 120, height = 120;
static int draw_result, present_result;
static int open_display(void* user) {
  (void)user;
  ++opened;
  return 0;
}
static int info(void* user, h2_display_info_t* out) {
  (void)user;
  *out = (h2_display_info_t){.width = width,
                             .height = height,
                             .native_format = H2_DISPLAY_PIXEL_RGB565};
  return 0;
}
static int brightness(void* user, uint32_t percent) {
  (void)user;
  assert(percent >= 10 && percent <= 100);
  return 0;
}
static int draw(void* user, const h2_display_rect_t* area, const void* pixels,
                size_t stride, h2_display_pixel_format_t format) {
  (void)user;
  assert(area->width == width && area->height == height && pixels != NULL);
  assert(stride == width * 2u && format == H2_DISPLAY_PIXEL_RGB565);
  return draw_result;
}
static int present(void* user) {
  (void)user;
  ++presented;
  return present_result;
}
static int close_display(void* user) {
  (void)user;
  ++closed;
  return 0;
}

static uint16_t pixel(const h2_mfg_t* m, unsigned x, unsigned y) {
  return m->frame[(size_t)y * width + x];
}
static unsigned colored(const h2_mfg_t* m, unsigned x, unsigned y, unsigned w,
                        unsigned h) {
  unsigned count = 0;
  for (unsigned row = y; row < y + h; ++row)
    for (unsigned col = x; col < x + w; ++col) count += pixel(m, col, row) != 0;
  return count;
}
static void factory_tests(h2_runtime_t* rt) {
  width = height = 480;
  h2_mfg_step_t steps[] = {{"display", H2_MFG_DISPLAY, 0},
                           {"magnet", H2_MFG_MAGNETOMETERS, 0},
                           {"camera", H2_MFG_CAMERA, 0},
                           {"touch", H2_MFG_TOUCH, 0}};
  h2_mfg_profile_t profile = {.board = "test",
                              .steps = steps,
                              .total = 4,
                              .factory_ui = 1,
                              .sequential_touch_grid = 1};
  /* Guard both ends of the raw frame, including for edge touch cursors. */
  uint16_t* allocation = calloc(480u * 480u + 2u, sizeof(uint16_t));
  assert(allocation != NULL);
  allocation[0] = allocation[480u * 480u + 1u] = 0x1234;
  h2_mfg_t m = {
      .rt = rt,
      .profile = &profile,
      .frame = allocation + 1,
      .display_open = 1,
      .display = {.width = 480, .height = 480},
      .model = {.profile = &profile,
                .current = 4,
                .summary = {.total = 4,
                            .step_status = {H2_LOADER_MFG_STEP_PASSED,
                                            H2_LOADER_MFG_STEP_FAILED,
                                            H2_LOADER_MFG_STEP_SKIPPED,
                                            H2_LOADER_MFG_STEP_UNTESTED}}}};
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  /* Results stay in sequence; skipped/unset never turn green. */
  assert(pixel(&m, 210, 114) == 0x07e0);
  assert(pixel(&m, 230, 114) == 0xf800);
  assert(pixel(&m, 250, 114) == 0x4208);
  assert(pixel(&m, 270, 114) == 0x4208);
  assert(colored(&m, 150, 66, 180, 30) > 100);  /* 测试完成 */
  assert(colored(&m, 140, 220, 200, 30) > 100); /* 等待固件刷入 */
  assert(pixel(&m, 138, 396) == 0xfa60);
  assert(pixel(&m, 341, 443) == 0xfa60);
  assert(pixel(&m, 137, 396) == 0 && pixel(&m, 342, 444) == 0);
  assert(colored(&m, 180, 407, 120, 29) > 100);
  /* Incomplete traversal has red text; all-passed uses the shared gate. */
  unsigned red = 0;
  for (unsigned y = 272; y < 302; ++y)
    for (unsigned x = 100; x < 380; ++x) red += pixel(&m, x, y) == 0xf800;
  assert(red > 10);
  memset(m.model.summary.step_status, H2_LOADER_MFG_STEP_PASSED, 4);
  assert(h2_mfg_ui_draw(&m, 0) == 0 && h2_mfg_model_complete(&m.model));
  unsigned green = 0;
  for (unsigned y = 272; y < 302; ++y)
    for (unsigned x = 100; x < 380; ++x) green += pixel(&m, x, y) == 0x07e0;
  assert(green > 10);

  /* Real Chinese glyphs must be distinct A4 outlines, never ASCII fallback. */
  lv_font_glyph_dsc_t a = {0}, b = {0};
  assert(lv_font_get_glyph_dsc(&h2_mfg_factory_font, &a, 0x6d4b, 0)); /* 测 */
  assert(lv_font_get_glyph_dsc(&h2_mfg_factory_font, &b, 0x8bd5, 0)); /* 试 */
  assert(!a.is_placeholder && !b.is_placeholder && a.box_w >= 15 &&
         b.box_w >= 15);
  assert(a.format == LV_FONT_GLYPH_FORMAT_A4 &&
         b.format == LV_FONT_GLYPH_FORMAT_A4);
  a.req_raw_bitmap = b.req_raw_bitmap = 1;
  const uint8_t* a_bitmap = a.resolved_font->get_glyph_bitmap(&a, NULL);
  const uint8_t* b_bitmap = b.resolved_font->get_glyph_bitmap(&b, NULL);
  assert(a_bitmap != NULL && b_bitmap != NULL && a_bitmap != b_bitmap);
  lv_font_glyph_release_draw_data(&a);
  lv_font_glyph_release_draw_data(&b);

  m.model.current = 0;
  m.model.summary.step_status[0] = H2_LOADER_MFG_STEP_UNTESTED;
  const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
  for (unsigned i = 0; i < 4; ++i) {
    m.display_pattern = i;
    assert(h2_mfg_ui_draw(&m, 0) == 0);
    assert(pixel(&m, 100, 180) == colors[i]);
    assert(pixel(&m, 379, 299) == colors[i]);
    assert(pixel(&m, 99, 180) == 0 && pixel(&m, 380, 300) == 0);
  }
  assert(m.display_patterns_seen == 15u);
  m.display_patterns_seen = 0;
  draw_result = H2_PAL_ERR_IO;
  assert(h2_mfg_ui_draw(&m, 0) == H2_PAL_ERR_IO && !m.display_patterns_seen);
  draw_result = 0;
  present_result = H2_PAL_ERR_IO;
  assert(h2_mfg_ui_draw(&m, 0) == H2_PAL_ERR_IO && !m.display_patterns_seen);
  present_result = 0;
  m.model.ready = 0;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, 252, 396) == 0x3186);
  m.model.ready = 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0 && pixel(&m, 252, 396) == 0xfa60);

  m.model.current = 1;
  m.model.summary.step_status[1] = H2_LOADER_MFG_STEP_UNTESTED;
  m.magnet_present[1] = 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, 100, 220) == 0xfa60 && pixel(&m, 320, 220) == 0);
  m.magnet_present[1] = 0;
  m.magnet_present[0] = 1;
  m.magnet_error[1] = H2_PAL_ERR_IO;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, 60, 180) == 0xf800 && pixel(&m, 100, 220) == 0);
  assert(pixel(&m, 320, 220) == 0xfa60);

  uint16_t preview[12] = {0};
  for (unsigned i = 0; i < 12; ++i) preview[i] = (uint16_t)(0x1230u + i);
  m.camera_preview = preview;
  m.preview_width = 4;
  m.preview_height = 3;
  m.model.current = 2;
  m.model.summary.step_status[2] = H2_LOADER_MFG_STEP_UNTESTED;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, 48, 137) == preview[0] && pixel(&m, 51, 139) == preview[11]);
  assert(pixel(&m, 52, 139) == 0);
  m.model.current = 4;
  assert(h2_mfg_ui_draw(&m, 0) == 0 && pixel(&m, 48, 137) == 0);

  m.model.current = 3;
  m.model.summary.step_status[3] = H2_LOADER_MFG_STEP_UNTESTED;
  m.touch_target = 8;
  m.touch_hits = 0xff;
  m.touch_pressed = 1;
  m.touch_x = m.touch_y = 0;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  for (unsigned i = 0; i < 9; ++i) {
    int x, y;
    h2_mfg_touch_target(&m, i, &x, &y);
    assert(x == 80 + (int)(i % 3u) * 160 && y == 168 + (int)(i / 3u) * 120);
    assert(pixel(&m, (unsigned)x - 18, (unsigned)y - 18) ==
           (i == 8 ? 0xfa60 : 0x07e0));
  }
  assert(allocation[0] == 0x1234 && allocation[480u * 480u + 1u] == 0x1234);
  free(allocation);
}

int main(void) {
  const h2_pal_display_vtable_t vt = {.open = open_display,
                                      .get_info = info,
                                      .draw_bitmap = draw,
                                      .present = present,
                                      .set_brightness_percent = brightness,
                                      .close = close_display};
  const h2_pal_display_api_t display = {.vtable = &vt};
  h2_runtime_t rt = {.mem = h2_desktop_platform_default_allocator(),
                     .display = &display};
  const h2_mfg_step_t step = {"display", H2_MFG_DISPLAY, 0};
  const h2_mfg_profile_t profile = {
      .board = "test", .steps = &step, .total = 1};
  h2_mfg_t m = {
      .rt = &rt,
      .profile = &profile,
      .model = {
          .profile = &profile,
          .current = 1,
          .summary = {.total = 1, .step_status = {H2_LOADER_MFG_STEP_PASSED}}}};
  assert(h2_mfg_hardware_enter(&m, 100) == 0);
  assert(opened == 1 && m.frame != NULL);
  assert(h2_mfg_ui_draw(&m, 100) == 0 && presented == 1);
  assert(h2_mfg_hardware_close(&m) == 0 && closed == 1 && m.frame == NULL);
  factory_tests(&rt);
  return 0;
}
