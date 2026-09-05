#include "h2_button_smoke.h"

#include "h2_lvgl_platform.h"
#include "lvgl.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum { H2_BUTTON_SMOKE_MAX_BUTTONS = 16 };

typedef struct h2_button_smoke_item {
  lv_obj_t *object;
  lv_obj_t *label;
  uint32_t down_count;
  uint32_t action_count;
  int supported;
  int pressed;
} h2_button_smoke_item_t;

typedef struct h2_button_smoke_state {
  h2_runtime_t *runtime;
  const h2_button_smoke_config_t *config;
  void *render_buffer;
  lv_display_t *display;
  lv_obj_t *summary_label;
  h2_button_smoke_item_t items[H2_BUTTON_SMOKE_MAX_BUTTONS];
  h2_pal_result_t flush_result;
  uint64_t last_tick_ms;
  uint32_t total_events;
  int display_opened;
  int platform_initialized;
  int lvgl_initialized;
} h2_button_smoke_state_t;

static void button_smoke_flush(lv_display_t *display, const lv_area_t *area,
                               uint8_t *pixels) {
  h2_button_smoke_state_t *state = lv_display_get_user_data(display);
  h2_pal_result_t result = H2_PAL_ERR_INVALID_STATE;
  if (state != NULL && area != NULL && pixels != NULL) {
    const h2_display_rect_t rect = {
        .x = area->x1,
        .y = area->y1,
        .width = area->x2 - area->x1 + 1,
        .height = area->y2 - area->y1 + 1,
    };
    result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
        state->runtime->display, &rect, pixels,
        (size_t)rect.width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
    if (result == H2_PAL_OK) {
      result =
          (h2_pal_result_t)h2_pal_display_present(state->runtime->display);
    }
    if (state->flush_result == H2_PAL_OK && result != H2_PAL_OK) {
      state->flush_result = result;
    }
  }
  lv_display_flush_ready(display);
}

static void update_summary(h2_button_smoke_state_t *state) {
  char text[96];
  (void)snprintf(text, sizeof(text), "Runtime Button events: %u",
                 state->total_events);
  lv_label_set_text(state->summary_label, text);
}

static void update_item(h2_button_smoke_state_t *state, size_t index) {
  h2_button_smoke_item_t *item = &state->items[index];
  char text[96];
  if (!item->supported) {
    (void)snprintf(text, sizeof(text), "%s\nN/A",
                   state->config->buttons[index].name);
    lv_obj_set_style_bg_color(item->object, lv_color_hex(0x475569u),
                              LV_PART_MAIN);
  } else {
    (void)snprintf(text, sizeof(text), "%s\n%s  #%u",
                   state->config->buttons[index].name,
                   item->pressed ? "DOWN" : "ready", item->down_count);
    lv_obj_set_style_bg_color(
        item->object,
        lv_color_hex(item->pressed ? 0x16a34au : 0x2563ebu), LV_PART_MAIN);
  }
  lv_label_set_text(item->label, text);
}

static h2_button_smoke_item_t *find_item(
    h2_button_smoke_state_t *state,
    h2_runtime_component_id_t component_id) {
  for (size_t i = 0u; i < state->config->button_count; ++i) {
    if (state->config->buttons[i].component_id == component_id) {
      return &state->items[i];
    }
  }
  return NULL;
}

static void process_events(h2_button_smoke_state_t *state) {
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
  while (h2_runtime_poll_event(state->runtime, &event) == H2_PAL_OK) {
    h2_button_smoke_item_t *item = find_item(state, event.component_id);
    if (item == NULL) continue;
    const char *kind = NULL;
    if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN) {
      item->pressed = 1;
      ++item->down_count;
      kind = "down";
    } else if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP) {
      item->pressed = 0;
      kind = "up";
    } else if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION) {
      ++item->action_count;
      kind = "action";
    }
    if (kind == NULL) continue;
    ++state->total_events;
    size_t index = (size_t)(item - state->items);
    update_item(state, index);
    update_summary(state);
    char message[128];
    (void)snprintf(message, sizeof(message),
                   "button=%s component=%u event=%s down=%u action=%u",
                   state->config->buttons[index].name,
                   (unsigned)event.component_id, kind, item->down_count,
                   item->action_count);
    (void)h2_pal_log_write(state->runtime->log, H2_PAL_LOG_INFO,
                           "button-smoke", message);
  }
}

static h2_pal_result_t ui_init(h2_button_smoke_state_t *state) {
  if (h2_pal_display_open(state->runtime->display) != H2_DISPLAY_OK) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  state->display_opened = 1;
  h2_display_info_t info = {0};
  if (h2_pal_display_get_info(state->runtime->display, &info) !=
          H2_DISPLAY_OK ||
      info.width != (int)state->config->width ||
      info.height != (int)state->config->height) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const size_t frame_bytes = (size_t)state->config->width *
                             state->config->height * sizeof(uint16_t);
  state->render_buffer = h2_pal_mem_alloc(state->runtime->mem, frame_bytes);
  if (state->render_buffer == NULL) return H2_PAL_ERR_NO_MEMORY;
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
  state->display = lv_display_create((int32_t)state->config->width,
                                     (int32_t)state->config->height);
  if (state->display == NULL) return H2_PAL_ERR_NO_MEMORY;
  lv_display_set_default(state->display);
  lv_display_set_user_data(state->display, state);
  lv_display_set_color_format(state->display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(state->display, button_smoke_flush);
  lv_display_set_buffers(state->display, state->render_buffer, NULL,
                         (uint32_t)frame_bytes, LV_DISPLAY_RENDER_MODE_FULL);

  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0f172au), LV_PART_MAIN);
  lv_obj_t *title = lv_label_create(screen);
  state->summary_label = lv_label_create(screen);
  if (title == NULL || state->summary_label == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_label_set_text(title, "Runtime Button Monitor");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
  update_summary(state);
  lv_obj_align(state->summary_label, LV_ALIGN_TOP_MID, 0, 38);

  const int32_t columns = 4;
  const int32_t rows = (int32_t)((state->config->button_count + 3u) / 4u);
  const int32_t gap = 8;
  const int32_t left = 12;
  const int32_t top = 70;
  const int32_t cell_width =
      ((int32_t)state->config->width - left * 2 - gap * (columns - 1)) /
      columns;
  const int32_t cell_height =
      ((int32_t)state->config->height - top - 12 - gap * (rows - 1)) / rows;
  for (size_t i = 0u; i < state->config->button_count; ++i) {
    h2_runtime_component_info_t component = {0};
    state->items[i].supported =
        h2_runtime_component_get(
            state->runtime, state->config->buttons[i].component_id,
            &component) == H2_PAL_OK &&
        component.kind == H2_RUNTIME_COMPONENT_BUTTON;
    state->items[i].object = lv_obj_create(screen);
    if (state->items[i].object == NULL) return H2_PAL_ERR_NO_MEMORY;
    lv_obj_set_size(state->items[i].object, cell_width, cell_height);
    lv_obj_set_pos(state->items[i].object,
                   left + (int32_t)(i % 4u) * (cell_width + gap),
                   top + (int32_t)(i / 4u) * (cell_height + gap));
    lv_obj_set_clickable(state->items[i].object, false);
    state->items[i].label = lv_label_create(state->items[i].object);
    if (state->items[i].label == NULL) return H2_PAL_ERR_NO_MEMORY;
    lv_obj_center(state->items[i].label);
    update_item(state, i);
  }
  return H2_PAL_OK;
}

static void ui_deinit(h2_button_smoke_state_t *state) {
  if (state->display != NULL) lv_display_delete(state->display);
  if (state->lvgl_initialized) lv_deinit();
  if (state->platform_initialized) h2_lvgl_platform_deinit();
  h2_pal_mem_free(state->runtime->mem, state->render_buffer);
  if (state->display_opened) {
    (void)h2_pal_display_close(state->runtime->display);
  }
}

h2_pal_result_t h2_button_smoke_run(
    h2_runtime_t *runtime, const h2_button_smoke_config_t *config) {
  if (runtime == NULL || config == NULL || config->width == 0u ||
      config->height == 0u || config->buttons == NULL ||
      config->button_count == 0u ||
      config->button_count > H2_BUTTON_SMOKE_MAX_BUTTONS ||
      config->should_stop == NULL || runtime->mem == NULL ||
      runtime->log == NULL || runtime->display == NULL ||
      runtime->time == NULL || runtime->task == NULL ||
      runtime->sync == NULL || runtime->queue == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (config->width > (uint32_t)INT_MAX ||
      config->height > (uint32_t)INT_MAX ||
      (size_t)config->width > SIZE_MAX / config->height ||
      (size_t)config->width * config->height > SIZE_MAX / sizeof(uint16_t)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_button_smoke_state_t state;
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.config = config;
  state.flush_result = H2_PAL_OK;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(runtime->time, &state.last_tick_ms);
  if (result == H2_PAL_OK) result = ui_init(&state);
  if (config->on_started != NULL) {
    config->on_started(config->started_user, result);
  }
  while (result == H2_PAL_OK && !config->should_stop(config->stop_user)) {
    process_events(&state);
    uint64_t now_ms = state.last_tick_ms;
    result = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    if (result != H2_PAL_OK) break;
    uint64_t elapsed_ms =
        now_ms >= state.last_tick_ms ? now_ms - state.last_tick_ms : 0u;
    if (elapsed_ms > UINT32_MAX) elapsed_ms = UINT32_MAX;
    state.last_tick_ms = now_ms;
    lv_tick_inc((uint32_t)elapsed_ms);
    (void)lv_timer_handler();
    result = state.flush_result;
    if (result == H2_PAL_OK) {
      result = h2_pal_time_sleep_ms(runtime->time, 8u);
    }
  }
  ui_deinit(&state);
  return result;
}
