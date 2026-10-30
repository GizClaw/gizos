#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static unsigned opens, closes;
static h2_pal_touch_event_t events[2];
static unsigned event_at;
static int open_display(void* user) {
  (void)user;
  return 0;
}
static int display_info(void* user, h2_display_info_t* out) {
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
static h2_pal_result_t open_touch(void* user) {
  (void)user;
  ++opens;
  return 0;
}
static h2_pal_result_t touch_info(void* user, h2_pal_touch_info_t* out) {
  (void)user;
  *out = (h2_pal_touch_info_t){.width = 120, .height = 120};
  return 0;
}
static h2_pal_result_t poll(void* user, h2_pal_touch_event_t* out) {
  (void)user;
  if (event_at == 2) return H2_PAL_ERR_WOULD_BLOCK;
  *out = events[event_at++];
  return 0;
}
static h2_pal_result_t close_touch(void* user) {
  (void)user;
  ++closes;
  return 0;
}
static void click(int x) {
  events[0] =
      (h2_pal_touch_event_t){.kind = H2_PAL_TOUCH_EVENT_DOWN, .x = x, .y = 106};
  events[1] =
      (h2_pal_touch_event_t){.kind = H2_PAL_TOUCH_EVENT_UP, .x = x, .y = 106};
  event_at = 0;
}

int main(void) {
  const h2_pal_display_vtable_t display_vt = {
      .open = open_display,
      .get_info = display_info,
      .set_brightness_percent = brightness,
      .close = open_display};
  const h2_pal_display_api_t display = {.vtable = &display_vt};
  const h2_pal_touch_vtable_t touch_vt = {.open = open_touch,
                                          .get_info = touch_info,
                                          .poll_event = poll,
                                          .close = close_touch};
  const h2_pal_touch_api_t touch = {.vtable = &touch_vt};
  h2_runtime_t rt = {.mem = h2_desktop_platform_default_allocator(),
                     .display = &display,
                     .touch = &touch};
  const h2_mfg_step_t steps[] = {{"display", H2_MFG_DISPLAY, 0},
                                 {"touch", H2_MFG_TOUCH, 0},
                                 {"aec", H2_MFG_AEC, 0}};
  const h2_mfg_profile_t profile = {
      .board = "test", .steps = steps, .total = 3};
  h2_mfg_t m = {
      .rt = &rt,
      .profile = &profile,
      .model = {.profile = &profile,
                .current = 2,
                .summary = {.total = 3,
                            .step_status = {H2_LOADER_MFG_STEP_PASSED,
                                            H2_LOADER_MFG_STEP_PASSED,
                                            H2_LOADER_MFG_STEP_FAILED}}}};
  /* A cold boot resumes directly at failed AEC. It never enters TOUCH, but
   * the displayed RETRY and FAIL controls must still receive touch events. */
  assert(h2_mfg_hardware_enter(&m, 0) == 0);
  assert(opens == 1 && m.touch_open);
  const char* action = NULL;
  click(30);
  assert(h2_mfg_hardware_tick(&m, 20, &action) == 0);
  assert(action != NULL && strcmp(action, "retry") == 0);
  action = NULL;
  click(90);
  assert(h2_mfg_hardware_tick(&m, 40, &action) == 0);
  assert(action != NULL && strcmp(action, "fail") == 0);
  assert(h2_mfg_hardware_enter(&m, 60) == 0 && opens == 1);
  assert(h2_mfg_hardware_close(&m) == 0 && closes == 1);
  return 0;
}
