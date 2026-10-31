#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static unsigned opens, closes;
static h2_pal_touch_event_t events[2];
static int save_result;
static int persist(void* user, const h2_loader_mfg_summary_t* summary) {
  (void)user;
  (void)summary;
  return save_result;
}
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

static int button_state[2];
static int button_read(void* user, h2_pal_periph_id_t id,
                       h2_pal_single_button_reading_t* out) {
  (void)user;
  assert(id == 101 || id == 102);
  *out = (h2_pal_single_button_reading_t){
      .id = id,
      .state = button_state[id - 101] ? H2_PAL_BUTTON_STATE_PRESSED
                                      : H2_PAL_BUTTON_STATE_RELEASED};
  return 0;
}
static void ordered_button_edges_and_skip(void) {
  static const h2_pal_button_vtable_t ops = {.read_single_button = button_read};
  const h2_pal_button_api_t button = {.vtable = &ops};
  h2_runtime_t rt = {.button = &button};
  const h2_mfg_step_t step = {"buttons", H2_MFG_BUTTONS, 0};
  const h2_mfg_profile_t profile = {.steps = &step,
                                    .total = 1,
                                    .buttons = {101, 102},
                                    .button_count = 2,
                                    .touch_navigation = 1,
                                    .sequential_buttons = 1};
  h2_mfg_t m = {.rt = &rt, .profile = &profile};
  const char* action = NULL;
  /* BOOT held before AI must neither pass BOOT nor form an AI-then-BOOT skip.
   */
  button_state[1] = 1;
  assert(h2_mfg_hardware_tick(&m, 0, &action) == 0 && !action);
  button_state[0] = 1;
  assert(h2_mfg_hardware_tick(&m, 1, &action) == 0 && !action);
  button_state[0] = 0;
  assert(h2_mfg_hardware_tick(&m, 2, &action) == 0 && m.button_index == 1);
  button_state[1] = 0;
  assert(h2_mfg_hardware_tick(&m, 3, &action) == 0 && !action &&
         m.button_index == 1);
  button_state[1] = 1;
  assert(h2_mfg_hardware_tick(&m, 4, &action) == 0 && !action);
  button_state[1] = 0;
  assert(h2_mfg_hardware_tick(&m, 5, &action) == 0 && action &&
         !strcmp(action, "confirm"));
  m.button_index = 0;
  action = NULL;
  button_state[0] = 1;
  assert(h2_mfg_hardware_tick(&m, 6, &action) == 0);
  button_state[1] = 1;
  assert(h2_mfg_hardware_tick(&m, 7, &action) == 0);
  button_state[1] = 0;
  assert(h2_mfg_hardware_tick(&m, 8, &action) == 0 && action &&
         !strcmp(action, "skip"));
  button_state[0] = 0;
}

int main(void) {
  ordered_button_edges_and_skip();
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
  /* Completed sessions still open touch, and a fresh tap requests the same
   * ticketed durable reset used by commands without indexing steps[total]. */
  h2_mfg_profile_t factory = profile;
  factory.touch_navigation = 1;
  m.profile = &factory;
  m.model.profile = &factory;
  m.model.current = factory.total;
  m.model.ticket = 7;
  m.model.save = persist;
  assert(h2_mfg_hardware_enter(&m, 80) == 0 && m.touch_open);
  events[0] = (h2_pal_touch_event_t){
      .kind = H2_PAL_TOUCH_EVENT_DOWN, .x = 180, .y = 420};
  events[1] =
      (h2_pal_touch_event_t){.kind = H2_PAL_TOUCH_EVENT_UP, .x = 180, .y = 420};
  event_at = 0;
  /* This fake display is small: reject out-of-bounds input without touching
   * terminal-state result memory. */
  action = NULL;
  assert(h2_mfg_hardware_tick(&m, 100, &action) == H2_PAL_ERR_FORMAT);
  assert(action == NULL && m.model.current == factory.total);
  m.display.width = m.display.height = 480;
  event_at = 0;
  assert(h2_mfg_hardware_tick(&m, 120, &action) == 0);
  assert(action && strcmp(action, "reset") == 0);
  save_result = H2_PAL_ERR_IO;
  assert(h2_mfg_model_action(&m.model, action, "all", 7) == H2_PAL_ERR_IO);
  assert(m.model.current == factory.total && m.model.ticket == 7);
  save_result = 0;
  assert(h2_mfg_model_action(&m.model, action, "all", 7) == 0);
  assert(m.model.current == 0 && m.model.ticket == 8);
  assert(h2_mfg_hardware_close(&m) == 0);
  factory.sequential_touch_grid = 1;
  m.model.current = 1;
  m.model.error = 0;
  m.model.summary.step_status[1] = H2_LOADER_MFG_STEP_UNTESTED;
  assert(h2_mfg_hardware_enter(&m, 140) == 0);
  m.display.width = m.display.height = 480;
  for (unsigned i = 0; i < 9; ++i) {
    int x, y;
    h2_mfg_touch_target(&m, i, &x, &y);
    events[0] =
        (h2_pal_touch_event_t){.kind = H2_PAL_TOUCH_EVENT_DOWN, .x = x, .y = y};
    events[1] = (h2_pal_touch_event_t){
        .kind = H2_PAL_TOUCH_EVENT_UP, .x = x + 40, .y = y};
    event_at = 0;
    action = NULL;
    assert(h2_mfg_hardware_tick(&m, 160 + i, &action) == 0 && !action);
    assert(m.touch_target == i);
    events[1].x = x;
    event_at = 0;
    assert(h2_mfg_hardware_tick(&m, 180 + i, &action) == 0);
    assert(m.touch_target == i + 1);
    assert(i == 8 ? action && !strcmp(action, "confirm") : action == NULL);
  }
  assert(m.touch_hits == 0x1ffu && m.model.ready);
  assert(h2_mfg_hardware_close(&m) == 0);
  return 0;
}
