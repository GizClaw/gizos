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
/* Independent presentation fixture: shared tests never depend on a board entry.
 */
static const h2_mfg_ui_config_t factory_ui = {
    .width = 480,
    .height = 480,
    .title = "Factory",
    .background = 0,
    .accent = 0xfa60,
    .text = 0xffff,
    .passed = 0x07e0,
    .failed = 0xf800,
    .muted = 0x4208,
    .disabled = 0x3186,
    .title_area = {24, 24, 432, 26},
    .heading = {24, 66, 432, 30},
    .progress = {24, 110, 432, 9},
    .controls = {{24, 396, 204, 48}, {252, 396, 204, 48}},
    .retest = {138, 396, 204, 48},
    .display_pattern = {100, 180, 280, 120},
    .display_note = {48, 328, 384, 30},
    .brightness_levels = {48, 174, 384, 100},
    .brightness_control = {48, 300, 384, 40},
    .brightness_note = {48, 350, 384, 30},
    .touch_targets = {{62, 150, 36, 36},
                      {222, 150, 36, 36},
                      {382, 150, 36, 36},
                      {62, 270, 36, 36},
                      {222, 270, 36, 36},
                      {382, 270, 36, 36},
                      {62, 390, 36, 36},
                      {222, 390, 36, 36},
                      {382, 390, 36, 36}},
    .touch_tolerance = 28,
    .rows = {{48, 144, 384, 30},
             {48, 185, 384, 30},
             {48, 225, 384, 30},
             {48, 265, 384, 30},
             {48, 314, 384, 30},
             {48, 345, 384, 30},
             {48, 362, 384, 30},
             {48, 137, 384, 30}},
    .magnet_boxes = {{60, 180, 144, 144}, {276, 180, 144, 144}},
    .magnet_labels = {{60, 142, 168, 30}, {276, 142, 168, 30}},
    .magnet_errors = {{52, 332, 184, 30}, {268, 332, 184, 30}},
    .magnet_order = {1, 0},
    .magnet_names = {"Left", "Right"},
    .vibration = {96, 168, 288, 160},
    .preview = {48, 137, 384, 216},
};

typedef struct magnet_observation {
  int present[H2_MFG_MAGNETOMETER_MAX];
  int error[H2_MFG_MAGNETOMETER_MAX];
  int result[H2_MFG_MAGNETOMETER_MAX];
  unsigned calls[H2_MFG_MAGNETOMETER_MAX];
} magnet_observation_t;

static int read_magnet(void* user, uint32_t index, int* present, int* error) {
  magnet_observation_t* observation = user;
  assert(index < H2_MFG_MAGNETOMETER_MAX);
  ++observation->calls[index];
  *present = observation->present[index];
  *error = observation->error[index];
  return observation->result[index];
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
                              .ui = &factory_ui,
                              .magnetometer_count = 2};
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
  assert(colored(&m, 150, 66, 180, 30) > 100); /* 测试完成 */
  assert(colored(&m, 140, 220, 200, 30) == 0); /* No waiting prompt. */
  assert(pixel(&m, 138, 396) == 0);
  assert(pixel(&m, 341, 443) == 0);
  assert(pixel(&m, 137, 396) == 0 && pixel(&m, 342, 444) == 0);
  assert(colored(&m, 180, 407, 120, 29) == 0); /* No retest control. */
  /* Incomplete traversal has red text; all-passed uses the shared gate. */
  unsigned red = 0;
  for (unsigned y = 265; y < 295; ++y)
    for (unsigned x = 100; x < 380; ++x) red += pixel(&m, x, y) == 0xf800;
  assert(red > 10);
  memset(m.model.summary.step_status, H2_LOADER_MFG_STEP_PASSED, 4);
  assert(h2_mfg_ui_draw(&m, 0) == 0 && h2_mfg_model_complete(&m.model));
  unsigned green = 0;
  for (unsigned y = 265; y < 295; ++y)
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
  for (unsigned i = 0; i < 4; ++i) {
    assert(h2_mfg_ui_draw(&m, i * 600u) == 0);
    assert(pixel(&m, 100, 180) == 0xf800);
    assert(pixel(&m, 379, 180) == 0x07e0);
    assert(pixel(&m, 100, 299) == 0x001f);
    assert(pixel(&m, 379, 299) == 0xffff);
    assert(pixel(&m, 99, 180) == 0 && pixel(&m, 380, 300) == 0);
  }
  draw_result = H2_PAL_ERR_IO;
  assert(h2_mfg_ui_draw(&m, 0) == H2_PAL_ERR_IO);
  draw_result = 0;
  present_result = H2_PAL_ERR_IO;
  assert(h2_mfg_ui_draw(&m, 0) == H2_PAL_ERR_IO);
  present_result = 0;
  m.model.ready = 0;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, 252, 396) == 0x3186);
  m.model.ready = 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0 && pixel(&m, 252, 396) == 0xfa60);

  m.model.current = 1;
  m.model.summary.step_status[1] = H2_LOADER_MFG_STEP_UNTESTED;
  m.magnetometers[0].mag_mgauss = (h2_pal_vec3_i32_t){1, 2, 3};
  m.magnetometers[1].mag_mgauss = (h2_pal_vec3_i32_t){4, 5, 6};
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, 60, 180) == 0 && pixel(&m, 276, 180) == 0);
  assert(colored(&m, 60, 180, 144, 144) > 20);
  assert(colored(&m, 276, 180, 144, 144) > 20);

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
  const h2_mfg_touch_point_t points[] = {{60, 170}, {210, 240}, {380, 310}};
  profile.touch_points = points;
  profile.touch_point_count = 3;
  m.touch_hits = 5;
  m.touch_pressed = 1;
  m.touch_x = m.touch_y = 0;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  for (unsigned i = 0; i < 3; ++i) {
    int x, y;
    h2_mfg_touch_target(&m, i, &x, &y);
    assert(x == points[i].x && y == points[i].y);
    assert(pixel(&m, (unsigned)x - 13, (unsigned)y - 13) ==
           (i == 1 ? 0xffff : 0x07e0));
  }
  assert(allocation[0] == 0x1234 && allocation[480u * 480u + 1u] == 0x1234);
  free(allocation);
}

static unsigned different(const h2_mfg_t* m, h2_mfg_ui_rect_t area,
                          uint16_t background) {
  unsigned count = 0;
  for (int y = area.y; y < area.y + area.height; ++y)
    for (int x = area.x; x < area.x + area.width; ++x)
      count += pixel(m, (unsigned)x, (unsigned)y) != background;
  return count;
}

static void configured_layout_tests(h2_runtime_t* rt) {
  /* Move every surface, change both dimensions and palette, and restore native
   * sensor order. Expectations come from fixture geometry and session state. */
  h2_mfg_ui_config_t ui = {
      .width = 360,
      .height = 400,
      .title = "Fixture B",
      .background = 0x0841,
      .accent = 0xffe0,
      .text = 0xf81f,
      .passed = 0x07ff,
      .failed = 0x8010,
      .muted = 0x632c,
      .disabled = 0x2104,
      .title_area = {20, 8, 320, 30},
      .heading = {20, 40, 320, 30},
      .progress = {20, 76, 320, 8},
      .controls = {{10, 354, 150, 36}, {180, 354, 170, 36}},
      .retest = {80, 354, 200, 36},
      .display_pattern = {40, 110, 240, 105},
      .display_note = {20, 228, 320, 30},
      .brightness_levels = {30, 105, 300, 90},
      .brightness_control = {30, 220, 300, 40},
      .brightness_note = {20, 280, 320, 30},
      .touch_targets = {{35, 100, 24, 30},
                        {160, 100, 30, 24},
                        {285, 100, 30, 30},
                        {35, 190, 30, 30},
                        {160, 190, 30, 30},
                        {285, 190, 30, 30},
                        {35, 280, 30, 30},
                        {160, 280, 30, 30},
                        {285, 280, 30, 30}},
      .rows = {{20, 96, 320, 30},
               {20, 126, 320, 30},
               {20, 156, 320, 30},
               {20, 186, 320, 30},
               {20, 216, 320, 30},
               {20, 246, 320, 30},
               {20, 276, 320, 30},
               {20, 306, 320, 30}},
      .magnet_boxes = {{30, 142, 120, 100}, {200, 142, 120, 100}},
      .magnet_labels = {{30, 105, 120, 30}, {200, 105, 120, 30}},
      .magnet_errors = {{20, 260, 150, 30}, {190, 260, 150, 30}},
      .magnet_order = {0, 1},
      .magnet_names = {"Sensor A", "Sensor B"},
      .vibration = {45, 120, 270, 100},
      .preview = {30, 110, 300, 210},
  };
  h2_mfg_step_t step = {"fixture", H2_MFG_DISPLAY, 0};
  h2_mfg_profile_t profile = {
      .board = "independent",
      .steps = &step,
      .total = 1,
      .ui = &ui,
      .button_count = H2_MFG_BUTTON_MAX,
      .button_names = {"KEY A", "KEY B", "KEY C", "KEY D", "KEY E", "KEY F",
                       "KEY G", "KEY H"},
      .magnetometer_count = 2,
  };
  width = (int)ui.width;
  height = (int)ui.height;
  size_t pixels = (size_t)width * height;
  uint16_t* allocation = calloc(pixels + 2u, sizeof(*allocation));
  assert(allocation != NULL);
  allocation[0] = allocation[pixels + 1u] = 0x1234;
  h2_mfg_t m = {
      .rt = rt,
      .profile = &profile,
      .frame = allocation + 1,
      .display_open = 1,
      .display = {.width = width, .height = height},
      .model = {.profile = &profile, .summary = {.total = 1}},
  };
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, 0, 0) == ui.background);
  assert(different(&m, ui.title_area, ui.background) > 20);
  ui.title = "";
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(different(&m, ui.title_area, ui.background) == 0);
  ui.title = "Fixture B";
  h2_mfg_ui_rect_t area = ui.display_pattern;
  assert(pixel(&m, area.x, area.y) == 0xf800);
  assert(pixel(&m, area.x + area.width - 1, area.y) == 0x07e0);
  assert(pixel(&m, area.x, area.y + area.height - 1) == 0x001f);
  assert(pixel(&m, area.x + area.width - 1, area.y + area.height - 1) ==
         0xffff);
  assert(pixel(&m, area.x - 1, area.y) == ui.background);
  assert(pixel(&m, ui.controls[0].x, ui.controls[0].y) == ui.accent);
  assert(pixel(&m, ui.controls[1].x, ui.controls[1].y) == ui.disabled);
  m.model.ready = 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.controls[1].x, ui.controls[1].y) == ui.accent);

  step.check = H2_MFG_TOUCH;
  /* Application coordinates are independent of legacy nine-target geometry. */
  const h2_mfg_touch_point_t points[] = {{44, 111}, {180, 204}, {309, 292}};
  profile.touch_points = points;
  profile.touch_point_count = 3;
  m.touch_hits = 1u;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  for (unsigned i = 0; i < 3; ++i) {
    int x, y;
    h2_mfg_touch_target(&m, i, &x, &y);
    assert(x == points[i].x && y == points[i].y);
    uint16_t expected = i == 0 ? ui.passed : ui.text;
    assert(pixel(&m, x - 13, y - 13) == expected);
    assert(pixel(&m, x + 13, y + 13) == expected);
  }

  h2_mfg_touch_point_t many_points[32];
  for (unsigned i = 0; i < 32; ++i)
    many_points[i] = (h2_mfg_touch_point_t){40 + (int)(i % 8u) * 40,
                                            110 + (int)(i / 8u) * 60};
  profile.touch_points = many_points;
  profile.touch_point_count = 32;
  m.touch_hits = 0x80000001u;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  for (unsigned i = 0; i < 32; ++i) {
    int x, y;
    h2_mfg_touch_target(&m, i, &x, &y);
    assert(x == many_points[i].x && y == many_points[i].y);
    assert(pixel(&m, x - 13, y - 13) ==
           (i == 0 || i == 31 ? ui.passed : ui.text));
  }
  /* Absent or empty application lists preserve the shared five positions. */
  static const int default_xy[5][2] = {
      {21, 25}, {79, 25}, {50, 50}, {21, 75}, {79, 75}};
  for (unsigned absent = 0; absent < 2; ++absent) {
    profile.touch_points = absent ? NULL : many_points;
    profile.touch_point_count = absent ? 32 : 0;
    for (unsigned i = 0; i < 5; ++i) {
      int x, y;
      h2_mfg_touch_target(&m, i, &x, &y);
      assert(x == width * default_xy[i][0] / 100);
      assert(y == height * default_xy[i][1] / 100);
    }
  }

  step.check = H2_MFG_BUTTONS;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  for (unsigned i = 0; i < H2_MFG_BUTTON_MAX; ++i)
    assert(different(&m, ui.rows[i], ui.background) > 20);
  uint16_t* saved = malloc(pixels * sizeof(*saved));
  assert(saved != NULL);
  memcpy(saved, m.frame, pixels * sizeof(*saved));
  profile.button_names[7] = "LAST";
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(memcmp(saved, m.frame, pixels * sizeof(*saved)) != 0);
  memcpy(saved, m.frame, pixels * sizeof(*saved));
  m.downs[7] = 3;
  m.ups[7] = 2;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(memcmp(saved, m.frame, pixels * sizeof(*saved)) != 0);
  profile.button_count = 1;
  profile.button_names[0] =
      "A VERY LONG BUTTON LABEL THAT MUST STAY INSIDE ITS CONFIGURED ROW";
  ui.rows[0] = (h2_mfg_ui_rect_t){width - 60, 96, 100, 30};
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(different(&m, (h2_mfg_ui_rect_t){width - 60, 96, 60, 30},
                   ui.background) > 20);
  assert(pixel(&m, width - 61, 100) == ui.background);
  assert(allocation[0] == 0x1234 && allocation[pixels + 1u] == 0x1234);
  ui.rows[0] = (h2_mfg_ui_rect_t){20, 96, 320, 30};

  step.check = H2_MFG_MAGNETOMETERS;
  magnet_observation_t observation = {.present = {1, 0}};
  ui.read_magnet = read_magnet;
  ui.user = &observation;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(observation.calls[0] == 1 && observation.calls[1] == 1);
  assert(pixel(&m, ui.magnet_boxes[0].x + 10, ui.magnet_boxes[0].y + 10) ==
         ui.accent);
  assert(pixel(&m, ui.magnet_boxes[1].x + 10, ui.magnet_boxes[1].y + 10) ==
         ui.background);
  memcpy(saved, m.frame, pixels * sizeof(*saved));
  ui.magnet_names[0] = "Other";
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(memcmp(saved, m.frame, pixels * sizeof(*saved)) != 0);
  observation.error[1] = H2_PAL_ERR_IO;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.magnet_boxes[1].x, ui.magnet_boxes[1].y) == ui.failed);
  assert(different(&m, ui.magnet_errors[1], ui.background) > 20);
  observation.error[1] = 0;
  observation.result[1] = H2_PAL_ERR_IO;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.magnet_boxes[1].x, ui.magnet_boxes[1].y) == ui.failed);
  /* Reorder presentation slots while retaining the caller's index mapping. */
  ui.magnet_order[0] = 1;
  ui.magnet_order[1] = 0;
  observation.result[1] = 0;
  observation.present[1] = 0;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.magnet_boxes[1].x + 10, ui.magnet_boxes[1].y + 10) ==
         ui.accent);
  assert(pixel(&m, ui.magnet_boxes[0].x + 10, ui.magnet_boxes[0].y + 10) ==
         ui.background);
  /* The callback provides borrowed observations; rendering never classifies
   * raw vectors or modifies acceptance/readiness. */
  int ready = m.model.ready;
  m.magnetometers[0].mag_mgauss = (h2_pal_vec3_i32_t){99999, -99999, 99999};
  assert(h2_mfg_ui_draw(&m, 0) == 0 && m.model.ready == ready);
  assert(observation.present[0] == 1 && observation.present[1] == 0);
  ui.read_magnet = NULL;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.magnet_boxes[1].x, ui.magnet_boxes[1].y) ==
         ui.background);
  assert(different(&m, ui.magnet_boxes[1], ui.background) > 20);
  step.check = H2_MFG_BATTERY;
  m.battery.flags = H2_PAL_BATTERY_PRESENT | H2_PAL_BATTERY_HAS_VOLTAGE_MV |
                    H2_PAL_BATTERY_HAS_PERCENT_X100;
  m.battery.voltage_mv = 3800;
  m.battery.percent_x100 = 6543;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  for (unsigned i = 0; i < 3; ++i)
    assert(different(&m, ui.rows[i], ui.background) > 20);
  assert(different(&m, ui.rows[3], ui.background) == 0);
  assert(different(&m, ui.rows[4], ui.background) == 0);
  memcpy(saved, m.frame, pixels * sizeof(*saved));
  m.battery.flags |= H2_PAL_BATTERY_HAS_CURRENT_MA;
  m.battery.current_ma = -123;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(memcmp(saved, m.frame, pixels * sizeof(*saved)) == 0);
  m.battery.flags = 0;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(memcmp(saved, m.frame, pixels * sizeof(*saved)) != 0);

  step.check = H2_MFG_AEC;
  m.model.ready = 0;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  for (unsigned i = 0; i < 4; ++i)
    assert(different(&m, ui.rows[i], ui.background) > 20);
  memcpy(saved, m.frame, pixels * sizeof(*saved));
  m.mic_level = 1234;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(memcmp(saved, m.frame, pixels * sizeof(*saved)) != 0);
  assert(pixel(&m, ui.controls[1].x, ui.controls[1].y) == ui.disabled);
  m.model.ready = 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.controls[1].x, ui.controls[1].y) == ui.accent);

  step.check = H2_MFG_BLE;
  m.model.ready = m.ble_probed = 0;
  m.model.ticket = 0x12345678;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(different(&m, ui.rows[0], ui.background) > 20);
  assert(different(&m, ui.rows[2], ui.background) > 20);
  memcpy(saved, m.frame, pixels * sizeof(*saved));
  m.ble_probed = 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(memcmp(saved, m.frame, pixels * sizeof(*saved)) != 0);
  assert(pixel(&m, ui.controls[1].x, ui.controls[1].y) == ui.disabled);
  m.model.error = H2_PAL_ERR_IO;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.controls[1].x, ui.controls[1].y) == ui.accent);
  m.model.error = 0;
  free(saved);

  step.check = H2_MFG_VIBRATION;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.vibration.x, ui.vibration.y) == ui.disabled);
  m.motor_duty = 50;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.vibration.x, ui.vibration.y) == ui.accent);

  step.check = H2_MFG_CAMERA;
  uint16_t preview[] = {0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666};
  m.camera_preview = preview;
  m.preview_width = 3;
  m.preview_height = 2;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.preview.x, ui.preview.y) == preview[0]);
  assert(pixel(&m, ui.preview.x + 2, ui.preview.y + 1) == preview[5]);
  assert(pixel(&m, ui.preview.x + 3, ui.preview.y + 1) == ui.background);
  m.preview_width = ui.preview.width + 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.preview.x, ui.preview.y) == ui.background);
  m.preview_width = 3;
  m.preview_height = ui.preview.height + 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.preview.x, ui.preview.y) == ui.background);
  m.preview_height = 2;
  ui.preview.y = height - 1;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.preview.x, ui.preview.y) == ui.background);
  ui.preview.y = 110;
  ui.preview.x = width - 2;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, ui.preview.x, ui.preview.y) == ui.background);
  ui.preview.x = -1;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(allocation[0] == 0x1234 && allocation[pixels + 1u] == 0x1234);

  m.model.current = profile.total;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(different(&m, ui.retest, ui.background) == 0);
  assert(different(&m, ui.controls[0], ui.background) == 0);
  assert(different(&m, ui.controls[1], ui.background) == 0);
  assert(pixel(&m, ui.retest.x - 1, ui.retest.y) == ui.background);
  ui.width++;
  assert(h2_mfg_ui_draw(&m, 0) == H2_PAL_ERR_UNSUPPORTED);
  free(allocation);
}

static void compact_defaults(h2_runtime_t* rt) {
  width = 320;
  height = 240;
  h2_mfg_step_t step = {"display", H2_MFG_DISPLAY, 0};
  h2_mfg_profile_t profile = {.steps = &step, .total = 1};
  h2_mfg_t m = {.rt = rt,
                .profile = &profile,
                .display = {.width = 320, .height = 240},
                .display_open = 1};
  m.frame = calloc((size_t)width * height, sizeof(*m.frame));
  assert(m.frame);
  for (unsigned i = 0; i < 4; ++i) assert(h2_mfg_ui_draw(&m, i * 600u) == 0);
  assert(pixel(&m, 0, 0) == 0xf800);
  assert(pixel(&m, 319, 0) == 0x07e0);
  assert(pixel(&m, 0, 239) == 0x001f);
  assert(pixel(&m, 319, 239) == 0xffff);
  step.check = H2_MFG_CAMERA;
  uint16_t preview[] = {0x1234, 0x5678, 0x9abc, 0xdef0, 0x4321, 0x8765};
  m.camera_preview = preview;
  m.preview_width = 3;
  m.preview_height = 2;
  assert(h2_mfg_ui_draw(&m, 0) == 0);
  assert(pixel(&m, (320 - 3) / 2, 240 * 22 / 100) == preview[0]);
  assert(pixel(&m, (320 - 3) / 2 + 2, 240 * 22 / 100 + 1) == preview[5]);
  free(m.frame);
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
  configured_layout_tests(&rt);
  compact_defaults(&rt);
  return 0;
}
