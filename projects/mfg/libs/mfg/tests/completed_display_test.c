#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"
#include <assert.h>

static unsigned opened, presented, closed;
static int open_display(void* user) {
  (void)user;
  ++opened;
  return 0;
}
static int info(void* user, h2_display_info_t* out) {
  (void)user;
  *out = (h2_display_info_t){
      .width = 120, .height = 120, .native_format = H2_DISPLAY_PIXEL_RGB565};
  return 0;
}
static int brightness(void* user, uint32_t percent) {
  (void)user;
  assert(percent == 100);
  return 0;
}
static int draw(void* user, const h2_display_rect_t* area, const void* pixels,
                size_t stride, h2_display_pixel_format_t format) {
  (void)user;
  assert(area->width == 120 && area->height == 120 && pixels != NULL);
  assert(stride == 240 && format == H2_DISPLAY_PIXEL_RGB565);
  return 0;
}
static int present(void* user) {
  (void)user;
  ++presented;
  return 0;
}
static int close_display(void* user) {
  (void)user;
  ++closed;
  return 0;
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
  return 0;
}
