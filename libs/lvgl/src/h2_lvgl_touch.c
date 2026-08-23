#include "h2_lvgl_touch.h"

#include <string.h>

struct h2_lvgl_touch {
  const h2_pal_touch_api_t *touch;
  const h2_pal_mem_api_t *allocator;
  lv_indev_t *indev;
  lv_point_t point;
  lv_indev_state_t state;
  h2_pal_result_t last_result;
  int opened;
};

static void lvgl_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
  h2_lvgl_touch_t *adapter = lv_indev_get_user_data(indev);
  if (adapter == NULL) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  h2_pal_touch_event_t event = {0};
  h2_pal_result_t result = h2_pal_touch_poll_event(adapter->touch, &event);
  if (result == H2_PAL_OK) {
    adapter->point.x = event.x;
    adapter->point.y = event.y;
    adapter->state = event.kind == H2_PAL_TOUCH_EVENT_UP
                         ? LV_INDEV_STATE_RELEASED
                         : LV_INDEV_STATE_PRESSED;
    adapter->last_result = H2_PAL_OK;
    data->continue_reading = true;
  } else if (result != H2_PAL_ERR_WOULD_BLOCK) {
    adapter->last_result = result;
    adapter->state = LV_INDEV_STATE_RELEASED;
  }
  data->point = adapter->point;
  data->state = adapter->state;
}

h2_pal_result_t h2_lvgl_touch_create(
    const h2_lvgl_touch_config_t *config,
    h2_lvgl_touch_t **out_touch) {
  if (out_touch == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_touch = NULL;
  if (config == NULL || config->touch == NULL || config->allocator == NULL ||
      config->display == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  h2_pal_result_t result = h2_pal_touch_open(config->touch);
  if (result != H2_PAL_OK) {
    return result;
  }
  h2_pal_touch_info_t info = {0};
  result = h2_pal_touch_get_info(config->touch, &info);
  if (result != H2_PAL_OK || info.width == 0u || info.height == 0u ||
      info.width != (uint32_t)lv_display_get_horizontal_resolution(
                        config->display) ||
      info.height != (uint32_t)lv_display_get_vertical_resolution(
                         config->display)) {
    (void)h2_pal_touch_close(config->touch);
    return result == H2_PAL_OK ? H2_PAL_ERR_INVALID_ARG : result;
  }

  h2_lvgl_touch_t *adapter =
      h2_pal_mem_alloc(config->allocator, sizeof(*adapter));
  if (adapter == NULL) {
    (void)h2_pal_touch_close(config->touch);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(adapter, 0, sizeof(*adapter));
  adapter->touch = config->touch;
  adapter->allocator = config->allocator;
  adapter->state = LV_INDEV_STATE_RELEASED;
  adapter->last_result = H2_PAL_OK;
  adapter->opened = 1;
  adapter->indev = lv_indev_create();
  if (adapter->indev == NULL) {
    (void)h2_pal_touch_close(config->touch);
    h2_pal_mem_free(config->allocator, adapter);
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_indev_set_type(adapter->indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(adapter->indev, config->display);
  lv_indev_set_user_data(adapter->indev, adapter);
  lv_indev_set_read_cb(adapter->indev, lvgl_touch_read);
  *out_touch = adapter;
  return H2_PAL_OK;
}

h2_pal_result_t h2_lvgl_touch_last_result(const h2_lvgl_touch_t *touch) {
  return touch == NULL ? H2_PAL_ERR_INVALID_ARG : touch->last_result;
}

void h2_lvgl_touch_destroy(h2_lvgl_touch_t *touch) {
  if (touch == NULL) {
    return;
  }
  if (touch->indev != NULL) {
    lv_indev_delete(touch->indev);
    touch->indev = NULL;
  }
  if (touch->opened) {
    (void)h2_pal_touch_close(touch->touch);
  }
  const h2_pal_mem_api_t *allocator = touch->allocator;
  memset(touch, 0, sizeof(*touch));
  h2_pal_mem_free(allocator, touch);
}
