#include "h2_linux_platform.h"

#include <assert.h>
#include <linux/input.h>
#include <stdio.h>

static void feed(uint16_t type, uint16_t code, int32_t value,
                 h2_pal_result_t expected, h2_pal_touch_event_t *event) {
  assert(h2_linux_evdev_touch_test_feed(type, code, value, event) ==
         expected);
}

static void test_type_a_single_contact(void) {
  const h2_linux_evdev_touch_config_t config = {
      .device_name = "gt9xxnew_ts",
      .width = 1024u,
      .height = 600u,
  };
  assert(h2_linux_configure_evdev_touch(&config) == H2_PAL_OK);
  assert(h2_linux_evdev_touch_test_set_axes(100, 1100, 200, 800) ==
         H2_PAL_OK);
  h2_pal_touch_info_t info = {0};
  assert(h2_pal_touch_get_info(h2_linux_evdev_touch_api(), &info) ==
         H2_PAL_OK);
  assert(info.width == 1024u && info.height == 600u);

  h2_pal_touch_event_t event = {0};
  feed(EV_KEY, BTN_TOUCH, 1, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_X, 600, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_Y, 500, H2_PAL_ERR_WOULD_BLOCK,
       &event);
  feed(EV_SYN, SYN_MT_REPORT, 0, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(event.x == 511 && event.y == 299);

  feed(EV_ABS, ABS_MT_POSITION_X, 1200, H2_PAL_ERR_WOULD_BLOCK,
       &event);
  feed(EV_ABS, ABS_MT_POSITION_Y, 1000, H2_PAL_ERR_WOULD_BLOCK,
       &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 1023 && event.y == 599);

  feed(EV_KEY, BTN_TOUCH, 0, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP);
  assert(event.x == 1023 && event.y == 599);
}

static void test_initial_press_requires_same_report_coordinates(void) {
  const h2_linux_evdev_touch_config_t config = {
      .device_name = "gt9xxnew_ts",
      .width = 1024u,
      .height = 600u,
  };
  assert(h2_linux_configure_evdev_touch(&config) == H2_PAL_OK);
  assert(h2_linux_evdev_touch_test_set_axes(0, 1023, 0, 599) == H2_PAL_OK);

  h2_pal_touch_event_t event = {0};
  feed(EV_ABS, ABS_MT_POSITION_X, 100, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_Y, 200, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_ERR_WOULD_BLOCK, &event);

  feed(EV_KEY, BTN_TOUCH, 1, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_ERR_WOULD_BLOCK, &event);

  feed(EV_ABS, ABS_MT_POSITION_X, 300, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_Y, 400, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_ERR_WOULD_BLOCK, &event);

  feed(EV_KEY, BTN_TOUCH, 0, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_ERR_WOULD_BLOCK, &event);

  feed(EV_KEY, BTN_TOUCH, 1, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_X, 500, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_Y, 300, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(event.x == 500 && event.y == 300);
}

static void test_single_axis_move_and_up_use_last_coordinates(void) {
  const h2_linux_evdev_touch_config_t config = {
      .device_name = "gt9xxnew_ts",
      .width = 1024u,
      .height = 600u,
  };
  assert(h2_linux_configure_evdev_touch(&config) == H2_PAL_OK);
  assert(h2_linux_evdev_touch_test_set_axes(0, 1023, 0, 599) == H2_PAL_OK);

  h2_pal_touch_event_t event = {0};
  feed(EV_KEY, BTN_TOUCH, 1, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_X, 100, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_Y, 200, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(event.x == 100 && event.y == 200);

  feed(EV_ABS, ABS_MT_POSITION_X, 300, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  assert(event.x == 300 && event.y == 200);

  feed(EV_ABS, ABS_MT_POSITION_Y, 400, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_KEY, BTN_TOUCH, 0, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP);
  assert(event.x == 300 && event.y == 400);
}

static void test_orientation_and_axis_validation(void) {
  const h2_linux_evdev_touch_config_t config = {
      .device_name = "gt9xxnew_ts",
      .width = 1024u,
      .height = 600u,
      .swap_xy = 1,
      .invert_x = 1,
      .invert_y = 1,
  };
  assert(h2_linux_configure_evdev_touch(&config) == H2_PAL_OK);
  assert(h2_linux_evdev_touch_test_set_axes(10, 10, 0, 599) ==
         H2_PAL_ERR_UNSUPPORTED);
  h2_pal_touch_event_t event = {0};
  assert(h2_linux_evdev_touch_test_feed(
             EV_KEY, BTN_TOUCH, 1, &event) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_linux_evdev_touch_test_set_axes(0, 1023, 0, 599) ==
         H2_PAL_OK);
  feed(EV_KEY, BTN_TOUCH, 1, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_X, 0, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_Y, 599, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(event.x == 0 && event.y == 1023);
  h2_pal_touch_info_t info = {0};
  assert(h2_pal_touch_get_info(h2_linux_evdev_touch_api(), &info) ==
         H2_PAL_OK);
  assert(info.width == 600u && info.height == 1024u);
}

static void test_event_node_name_discovery(void) {
  assert(h2_linux_evdev_touch_test_is_event_node_name("event0") != 0);
  assert(h2_linux_evdev_touch_test_is_event_node_name("event63") != 0);
  assert(h2_linux_evdev_touch_test_is_event_node_name("event64") != 0);
  assert(h2_linux_evdev_touch_test_is_event_node_name("event1048576") != 0);
  assert(h2_linux_evdev_touch_test_is_event_node_name("event") == 0);
  assert(h2_linux_evdev_touch_test_is_event_node_name("event7x") == 0);
  assert(h2_linux_evdev_touch_test_is_event_node_name("not-event7") == 0);
}

static void test_full_int32_axis_range(void) {
  const h2_linux_evdev_touch_config_t config = {
      .device_name = "gt9xxnew_ts",
      .width = 1024u,
      .height = 600u,
  };
  assert(h2_linux_configure_evdev_touch(&config) == H2_PAL_OK);
  assert(h2_linux_evdev_touch_test_set_axes(
             INT32_MIN, INT32_MAX, INT32_MIN, INT32_MAX) == H2_PAL_OK);
  h2_pal_touch_event_t event = {0};
  feed(EV_KEY, BTN_TOUCH, 1, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_X, 0, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_ABS, ABS_MT_POSITION_Y, 0, H2_PAL_ERR_WOULD_BLOCK, &event);
  feed(EV_SYN, SYN_REPORT, 0, H2_PAL_OK, &event);
  assert(event.x == 511 && event.y == 299);
}

static void test_invalid_config(void) {
  assert(h2_linux_configure_evdev_touch(NULL) == H2_PAL_ERR_INVALID_ARG);
  const h2_linux_evdev_touch_config_t config = {
      .device_name = "",
      .width = 1u,
      .height = 1u,
  };
  assert(h2_linux_configure_evdev_touch(&config) == H2_PAL_ERR_INVALID_ARG);
  h2_linux_evdev_touch_config_t oversized = config;
  oversized.device_name = "gt9xxnew_ts";
  oversized.width = UINT32_MAX;
  assert(h2_linux_configure_evdev_touch(&oversized) ==
         H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_type_a_single_contact();
  test_initial_press_requires_same_report_coordinates();
  test_single_axis_move_and_up_use_last_coordinates();
  test_orientation_and_axis_validation();
  test_event_node_name_discovery();
  test_full_int32_axis_range();
  test_invalid_config();
  puts("linux evdev touch tests passed");
  return 0;
}
