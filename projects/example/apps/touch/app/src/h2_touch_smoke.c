#include "h2_touch_smoke.h"

#include "h2_lvgl_button.h"
#include "h2_lvgl_platform.h"
#include "h2_lvgl_touch.h"
#include "lvgl.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct h2_touch_smoke_state {
  h2_runtime_t *runtime;
  const h2_touch_smoke_config_t *config;
  void *render_buffer;
  lv_display_t *display;
  h2_lvgl_touch_t *touch;
  h2_lvgl_button_t button;
  lv_obj_t *status_label;
  lv_obj_t *position_label;
  lv_obj_t *touch_marker;
  h2_pal_result_t flush_result;
  uint32_t down_count;
  uint32_t up_count;
  uint32_t action_count;
  uint64_t last_tick_ms;
  int display_opened;
  int platform_initialized;
  int lvgl_initialized;
} h2_touch_smoke_state_t;

static void touch_smoke_flush(lv_display_t *display, const lv_area_t *area,
                              uint8_t *pixels) {
  h2_touch_smoke_state_t *state = lv_display_get_user_data(display);
  h2_pal_result_t flush_result = H2_PAL_ERR_INVALID_STATE;
  if (state != NULL && area != NULL && pixels != NULL) {
    const h2_display_rect_t rect = {
        .x = area->x1,
        .y = area->y1,
        .width = area->x2 - area->x1 + 1,
        .height = area->y2 - area->y1 + 1,
    };
    flush_result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
        state->runtime->display, &rect, pixels,
        (size_t)rect.width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
    if (flush_result == H2_PAL_OK) {
      flush_result =
          (h2_pal_result_t)h2_pal_display_present(state->runtime->display);
    }
    if (state->flush_result == H2_PAL_OK && flush_result != H2_PAL_OK) {
      state->flush_result = flush_result;
    }
  }
  lv_display_flush_ready(display);
}

static void touch_smoke_update_status(h2_touch_smoke_state_t *state) {
  char text[160];
  (void)snprintf(text, sizeof(text),
                 "Runtime events  down=%u  up=%u  action=%u",
                 state->down_count, state->up_count, state->action_count);
  lv_label_set_text(state->status_label, text);
}

static void touch_smoke_log_event(h2_touch_smoke_state_t *state,
                                  const char *kind) {
  char message[160];
  (void)snprintf(message, sizeof(message),
                 "event=%s down=%u up=%u action=%u", kind,
                 state->down_count, state->up_count, state->action_count);
  (void)h2_pal_log_write(state->runtime->log, H2_PAL_LOG_INFO,
                         "touch-smoke", message);
}

static void touch_smoke_pointer_event(lv_event_t *event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
      code != LV_EVENT_RELEASED) {
    return;
  }
  h2_touch_smoke_state_t *state = lv_event_get_user_data(event);
  lv_indev_t *indev = lv_event_get_indev(event);
  if (state == NULL || indev == NULL) {
    return;
  }
  lv_point_t point;
  lv_indev_get_point(indev, &point);
  lv_obj_set_pos(state->touch_marker, point.x - 10, point.y - 10);
  lv_obj_set_hidden(state->touch_marker, false);

  char text[80];
  const char *kind = code == LV_EVENT_RELEASED ? "up" : "down";
  (void)snprintf(text, sizeof(text), "Touch %s  x=%d  y=%d", kind,
                 (int)point.x, (int)point.y);
  lv_label_set_text(state->position_label, text);
  if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED) {
    (void)h2_pal_log_write(state->runtime->log, H2_PAL_LOG_INFO,
                           "touch-smoke", text);
  }
}

static void touch_smoke_process_events(h2_touch_smoke_state_t *state) {
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
  int changed = 0;
  while (h2_runtime_poll_event(state->runtime, &event) == H2_PAL_OK) {
    if (event.component_id != H2_TOUCH_SMOKE_COMPONENT_ACTION_BUTTON) {
      continue;
    }
    if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN) {
      ++state->down_count;
      touch_smoke_log_event(state, "down");
      changed = 1;
    } else if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP) {
      ++state->up_count;
      touch_smoke_log_event(state, "up");
      changed = 1;
    } else if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION) {
      ++state->action_count;
      touch_smoke_log_event(state, "action");
      changed = 1;
    }
  }
  if (changed) {
    touch_smoke_update_status(state);
  }
}

static h2_pal_result_t touch_smoke_ui_init(h2_touch_smoke_state_t *state) {
  if (h2_pal_display_open(state->runtime->display) != H2_DISPLAY_OK) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  state->display_opened = 1;
  h2_display_info_t display_info = {0};
  if (h2_pal_display_get_info(state->runtime->display, &display_info) !=
          H2_DISPLAY_OK ||
      display_info.width != (int)state->config->width ||
      display_info.height != (int)state->config->height) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const size_t frame_bytes = (size_t)state->config->width *
                             state->config->height * sizeof(uint16_t);
  state->render_buffer = h2_pal_mem_alloc(state->runtime->mem, frame_bytes);
  if (state->render_buffer == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  const h2_lvgl_platform_config_t platform = {
      .allocator = state->runtime->mem,
      .task_api = state->runtime->task,
      .sync_api = state->runtime->sync,
      .queue_api = state->runtime->queue,
      .time_api = state->runtime->time,
  };
  if (h2_lvgl_platform_init(&platform) != 0) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  state->platform_initialized = 1;
  lv_init();
  state->lvgl_initialized = 1;
  state->display =
      lv_display_create((int32_t)state->config->width,
                        (int32_t)state->config->height);
  if (state->display == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_display_set_default(state->display);
  lv_display_set_user_data(state->display, state);
  lv_display_set_color_format(state->display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(state->display, touch_smoke_flush);
  lv_display_set_buffers(state->display, state->render_buffer, NULL,
                         (uint32_t)frame_bytes, LV_DISPLAY_RENDER_MODE_FULL);

  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0f172au), LV_PART_MAIN);
  lv_obj_set_clickable(screen, true);
  lv_obj_add_event_cb(screen, touch_smoke_pointer_event, LV_EVENT_ALL, state);
  lv_obj_t *title = lv_label_create(screen);
  state->status_label = lv_label_create(screen);
  state->position_label = lv_label_create(screen);
  lv_obj_t *button = lv_button_create(screen);
  state->touch_marker = lv_obj_create(screen);
  if (title == NULL || state->status_label == NULL ||
      state->position_label == NULL || button == NULL ||
      state->touch_marker == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_label_set_text(title, "K4B Touch PAL -> LVGL");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);
  touch_smoke_update_status(state);
  lv_obj_align(state->status_label, LV_ALIGN_TOP_MID, 0, 150);
  lv_label_set_text(state->position_label, "Touch idle");
  lv_obj_align(state->position_label, LV_ALIGN_TOP_MID, 0, 190);
  lv_obj_set_size(button, 520, 180);
  lv_obj_center(button);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x2563ebu), LV_PART_MAIN);
  lv_obj_set_event_bubble(button, true);
  lv_obj_set_size(state->touch_marker, 20, 20);
  lv_obj_set_style_radius(state->touch_marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->touch_marker, lv_color_hex(0xef4444u),
                            LV_PART_MAIN);
  lv_obj_set_style_border_width(state->touch_marker, 0, LV_PART_MAIN);
  lv_obj_set_clickable(state->touch_marker, false);
  lv_obj_set_hidden(state->touch_marker, true);
  lv_obj_t *button_label = lv_label_create(button);
  if (button_label == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_label_set_text(button_label, "Push Runtime button");
  lv_obj_center(button_label);
  h2_pal_result_t result = h2_lvgl_button_bind(
      &state->button, state->runtime,
      H2_TOUCH_SMOKE_COMPONENT_ACTION_BUTTON, button);
  if (result != H2_PAL_OK) {
    return result;
  }
  const h2_lvgl_touch_config_t touch_config = {
      .touch = state->runtime->touch,
      .allocator = state->runtime->mem,
      .display = state->display,
  };
  return h2_lvgl_touch_create(&touch_config, &state->touch);
}

static void touch_smoke_ui_deinit(h2_touch_smoke_state_t *state) {
  if (state->touch != NULL) {
    h2_lvgl_touch_destroy(state->touch);
    state->touch = NULL;
  }
  if (state->display != NULL) {
    lv_display_delete(state->display);
    state->display = NULL;
  }
  if (state->lvgl_initialized) {
    lv_deinit();
  }
  if (state->platform_initialized) {
    h2_lvgl_platform_deinit();
  }
  h2_pal_mem_free(state->runtime->mem, state->render_buffer);
  state->render_buffer = NULL;
  if (state->display_opened) {
    (void)h2_pal_display_close(state->runtime->display);
  }
}

h2_pal_result_t h2_touch_smoke_run(
    h2_runtime_t *runtime,
    const h2_touch_smoke_config_t *config) {
  if (runtime == NULL || config == NULL || config->width == 0u ||
      config->height == 0u ||
      config->should_stop == NULL || runtime->mem == NULL ||
      runtime->log == NULL || runtime->display == NULL ||
      runtime->touch == NULL ||
      runtime->time == NULL || runtime->task == NULL ||
      runtime->sync == NULL || runtime->queue == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (config->width > (uint32_t)INT_MAX ||
      config->height > (uint32_t)INT_MAX ||
      (size_t)config->width > SIZE_MAX / config->height ||
      (size_t)config->width * config->height > SIZE_MAX / sizeof(uint16_t) ||
      (size_t)config->width * config->height * sizeof(uint16_t) >
          UINT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_touch_smoke_state_t state;
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.config = config;
  state.flush_result = H2_PAL_OK;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(runtime->time, &state.last_tick_ms);
  if (result == H2_PAL_OK) {
    result = touch_smoke_ui_init(&state);
  }
  while (result == H2_PAL_OK && !config->should_stop(config->stop_user)) {
    touch_smoke_process_events(&state);
    uint64_t now_ms = state.last_tick_ms;
    result = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    if (result != H2_PAL_OK) {
      break;
    }
    uint64_t elapsed_ms = now_ms >= state.last_tick_ms
                              ? now_ms - state.last_tick_ms
                              : 0u;
    if (elapsed_ms > UINT32_MAX) {
      elapsed_ms = UINT32_MAX;
    }
    state.last_tick_ms = now_ms;
    lv_tick_inc((uint32_t)elapsed_ms);
    (void)lv_timer_handler();
    result = state.flush_result;
    if (result == H2_PAL_OK) {
      result = h2_lvgl_touch_last_result(state.touch);
    }
    if (result == H2_PAL_OK) {
      result = state.button.last_result;
    }
    if (result == H2_PAL_OK) {
      result = h2_pal_time_sleep_ms(runtime->time, 8u);
    }
  }
  touch_smoke_ui_deinit(&state);
  return result;
}
