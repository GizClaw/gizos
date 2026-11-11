#include "h2_lvgl_touch.h"

#include "h2_desktop_platform.h"
#include "h2_lvgl_platform.h"

#include <assert.h>
#include <string.h>

typedef struct touch_fixture {
  h2_pal_touch_event_t event;
  h2_pal_result_t result;
  unsigned int opens;
  unsigned int closes;
} touch_fixture_t;

static touch_fixture_t fixture;

static h2_pal_result_t touch_open(void* user) {
  (void)user;
  ++fixture.opens;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_info(void* user, h2_pal_touch_info_t* info) {
  (void)user;
  *info = (h2_pal_touch_info_t){40u, 30u};
  return H2_PAL_OK;
}

static h2_pal_result_t touch_read(void* user, h2_pal_touch_event_t* event) {
  (void)user;
  *event = fixture.event;
  return fixture.result;
}

static h2_pal_result_t touch_close(void* user) {
  (void)user;
  ++fixture.closes;
  return H2_PAL_OK;
}

int main(void) {
  const h2_pal_mem_api_t* allocator = h2_desktop_platform_default_allocator();
  const h2_lvgl_platform_config_t platform = {
      .allocator = allocator,
      .task_api = h2_desktop_platform_task_api(),
      .sync_api = h2_desktop_platform_sync_api(),
      .queue_api = h2_desktop_platform_queue_api(),
      .time_api = h2_desktop_platform_time_api()};
  assert(h2_lvgl_platform_init(&platform) == 0);
  lv_init();
  lv_display_t* display = lv_display_create(40, 30);
  assert(display != NULL);
  const h2_pal_touch_vtable_t vtable = {.open = touch_open,
                                        .get_info = touch_info,
                                        .poll_event = touch_read,
                                        .close = touch_close};
  const h2_pal_touch_api_t touch = {.vtable = &vtable};
  const h2_lvgl_touch_config_t config = {&touch, allocator, display};
  h2_lvgl_touch_t* adapter = NULL;
  assert(h2_lvgl_touch_create(&config, &adapter) == H2_PAL_OK);
  lv_indev_t* indev = lv_indev_get_next(NULL);
  assert(indev != NULL);
  lv_indev_read_cb_t read_callback = lv_indev_get_read_cb(indev);
  lv_indev_data_t data;
  memset(&data, 0, sizeof(data));
  fixture.event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 9, 8};
  read_callback(indev, &data);
  assert(data.state == LV_INDEV_STATE_RELEASED);
  fixture.event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 10, 12};
  read_callback(indev, &data);
  assert(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 10);
  assert(data.continue_reading);
  const h2_pal_result_t gaps[] = {H2_PAL_ERR_WOULD_BLOCK, H2_PAL_ERR_IO,
                                  H2_PAL_ERR_TIMEOUT};
  for (size_t index = 0u; index < sizeof(gaps) / sizeof(gaps[0]); ++index) {
    fixture.result = gaps[index];
    read_callback(indev, &data);
    assert(data.state == LV_INDEV_STATE_PRESSED);
    assert(data.point.x == 10 && data.point.y == 12);
    assert(!data.continue_reading);
    if (gaps[index] != H2_PAL_ERR_WOULD_BLOCK)
      assert(h2_lvgl_touch_last_result(adapter) == gaps[index]);
  }
  fixture.result = H2_PAL_OK;
  fixture.event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 11, 13};
  read_callback(indev, &data);
  assert(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 11);
  assert(h2_lvgl_touch_last_result(adapter) == H2_PAL_OK);
  fixture.event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 11, 13};
  read_callback(indev, &data);
  assert(data.state == LV_INDEV_STATE_RELEASED);
  fixture.event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 1, 2};
  read_callback(indev, &data);
  assert(data.state == LV_INDEV_STATE_RELEASED && data.point.x == 11);
  h2_lvgl_touch_destroy(adapter);
  assert(fixture.opens == 1u && fixture.closes == 1u);
  lv_display_delete(display);
  lv_deinit();
  h2_lvgl_platform_deinit();
  return 0;
}
