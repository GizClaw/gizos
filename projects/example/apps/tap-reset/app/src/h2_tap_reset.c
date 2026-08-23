#include "h2_tap_reset.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define H2_TAP_RESET_FRAME_BYTES                                               \
  ((size_t)H2_TAP_RESET_WIDTH * H2_TAP_RESET_HEIGHT * sizeof(uint16_t))

typedef struct h2_tap_reset_state {
  h2_runtime_t *runtime;
  const h2_tap_reset_config_t *config;
  lv_display_t *display;
  lv_indev_t *pointer;
  lv_obj_t *count_label;
  void *render_buffer;
  uint64_t last_tick_ms;
  uint32_t count;
  int display_opened;
  int lvgl_initialized;
} h2_tap_reset_state_t;

static uint32_t tap_reset_tick_get(void) {
  h2_tap_reset_state_t *state =
      (h2_tap_reset_state_t *)lv_display_get_user_data(
          lv_display_get_default());
  if (state == NULL || state->runtime == NULL) {
    return 0u;
  }
  uint64_t now_ms = state->last_tick_ms;
  if (h2_pal_time_get_monotonic_ms(state->runtime->time, &now_ms) ==
      H2_PAL_OK) {
    state->last_tick_ms = now_ms;
  }
  return (uint32_t)state->last_tick_ms;
}

static void tap_reset_flush(lv_display_t *display, const lv_area_t *area,
                            uint8_t *pixels) {
  h2_tap_reset_state_t *state =
      (h2_tap_reset_state_t *)lv_display_get_user_data(display);
  const h2_display_rect_t rect = {
      .x = area->x1,
      .y = area->y1,
      .width = area->x2 - area->x1 + 1,
      .height = area->y2 - area->y1 + 1,
  };
  if (state != NULL && state->runtime != NULL) {
    (void)h2_pal_display_draw_bitmap(state->runtime->display, &rect, pixels,
                                     (size_t)rect.width * sizeof(uint16_t),
                                     H2_DISPLAY_PIXEL_RGB565);
    (void)h2_pal_display_present(state->runtime->display);
  }
  lv_display_flush_ready(display);
}

static void tap_reset_pointer_read(lv_indev_t *indev, lv_indev_data_t *data) {
  h2_tap_reset_state_t *state =
      (h2_tap_reset_state_t *)lv_indev_get_user_data(indev);
  h2_tap_reset_pointer_state_t pointer = {0};
  if (state != NULL && state->config->read_pointer != NULL &&
      state->config->read_pointer(state->config->pointer_user, &pointer) ==
          H2_PAL_OK) {
    data->point.x = pointer.x;
    data->point.y = pointer.y;
    data->state =
        pointer.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void tap_reset_update_count(h2_tap_reset_state_t *state) {
  char text[32];
  (void)snprintf(text, sizeof(text), "%u taps", state->count);
  lv_label_set_text(state->count_label, text);
}

static void tap_reset_increment(lv_event_t *event) {
  h2_tap_reset_state_t *state =
      (h2_tap_reset_state_t *)lv_event_get_user_data(event);
  if (state != NULL) {
    ++state->count;
    tap_reset_update_count(state);
  }
}

static void tap_reset_reset(lv_event_t *event) {
  h2_tap_reset_state_t *state =
      (h2_tap_reset_state_t *)lv_event_get_user_data(event);
  if (state != NULL) {
    state->count = 0u;
    tap_reset_update_count(state);
  }
}

static void tap_reset_style_button(lv_obj_t *button, uint32_t color) {
  lv_obj_set_height(button, 58);
  lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
}

static int tap_reset_create_ui(h2_tap_reset_state_t *state) {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_scrollable(screen, false);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b1020u), LV_PART_MAIN);

  lv_obj_t *column = lv_obj_create(screen);
  if (column == NULL) {
    return 0;
  }
  lv_obj_set_size(column, 328, 560);
  lv_obj_center(column);
  lv_obj_set_scrollable(column, false);
  lv_obj_set_scrollbar_mode(column, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(column, 22, LV_PART_MAIN);
  lv_obj_set_style_pad_row(column, 14, LV_PART_MAIN);
  lv_obj_set_style_radius(column, 28, LV_PART_MAIN);
  lv_obj_set_style_border_width(column, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(column, lv_color_hex(0x334155u), LV_PART_MAIN);
  lv_obj_set_style_bg_color(column, lv_color_hex(0x111a2fu), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(column, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t *eyebrow = lv_label_create(column);
  lv_obj_t *title = lv_label_create(column);
  lv_obj_t *body = lv_label_create(column);
  state->count_label = lv_label_create(column);
  lv_obj_t *increment = lv_button_create(column);
  lv_obj_t *reset = lv_button_create(column);
  if (eyebrow == NULL || title == NULL || body == NULL ||
      state->count_label == NULL || increment == NULL || reset == NULL) {
    return 0;
  }

  lv_label_set_text(eyebrow, state->config->eyebrow);
  lv_obj_set_style_text_color(eyebrow, lv_color_hex(0x7dd3fcu), LV_PART_MAIN);
  lv_label_set_text(title, "Portable\nApp on\na phone");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_42, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0xf8fafcu), LV_PART_MAIN);
  lv_label_set_text(body, state->config->body);
  lv_obj_set_style_text_color(body, lv_color_hex(0x94a3b8u), LV_PART_MAIN);
  lv_obj_set_style_text_line_space(body, 7, LV_PART_MAIN);
  lv_obj_set_width(body, LV_PCT(100));

  lv_obj_set_style_text_font(state->count_label, &lv_font_montserrat_42,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(state->count_label, lv_color_hex(0xf8fafcu),
                              LV_PART_MAIN);
  tap_reset_update_count(state);

  lv_obj_set_width(increment, LV_PCT(100));
  tap_reset_style_button(increment, 0x2563ebu);
  lv_obj_add_event_cb(increment, tap_reset_increment, LV_EVENT_CLICKED, state);
  lv_obj_t *increment_label = lv_label_create(increment);
  lv_label_set_text(increment_label, state->config->increment_label);
  lv_obj_center(increment_label);

  lv_obj_set_width(reset, LV_PCT(100));
  tap_reset_style_button(reset, 0x334155u);
  lv_obj_add_event_cb(reset, tap_reset_reset, LV_EVENT_CLICKED, state);
  lv_obj_t *reset_label = lv_label_create(reset);
  lv_label_set_text(reset_label, "Reset");
  lv_obj_center(reset_label);
  return 1;
}

static void tap_reset_deinit(h2_tap_reset_state_t *state) {
  if (state->pointer != NULL) {
    lv_indev_delete(state->pointer);
    state->pointer = NULL;
  }
  if (state->display != NULL) {
    lv_display_delete(state->display);
    state->display = NULL;
  }
  if (state->lvgl_initialized) {
    lv_deinit();
  }
  h2_pal_mem_free(state->runtime->mem, state->render_buffer);
  if (state->display_opened) {
    (void)h2_pal_display_close(state->runtime->display);
  }
}

h2_pal_result_t h2_tap_reset_run(h2_runtime_t *runtime,
                                 const h2_tap_reset_config_t *config) {
  if (runtime == NULL || runtime->mem == NULL || runtime->display == NULL ||
      runtime->time == NULL || config == NULL || config->eyebrow == NULL ||
      config->body == NULL || config->increment_label == NULL ||
      config->read_pointer == NULL || config->should_stop == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  h2_tap_reset_state_t state;
  memset(&state, 0, sizeof(state));
  state.runtime = runtime;
  state.config = config;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(runtime->time, &state.last_tick_ms);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (h2_pal_display_open(runtime->display) != H2_DISPLAY_OK) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  state.display_opened = 1;

  h2_display_info_t info = {0};
  if (h2_pal_display_get_info(runtime->display, &info) != H2_DISPLAY_OK ||
      info.width != H2_TAP_RESET_WIDTH || info.height != H2_TAP_RESET_HEIGHT) {
    tap_reset_deinit(&state);
    return H2_PAL_ERR_INVALID_ARG;
  }

  state.render_buffer =
      h2_pal_mem_alloc(runtime->mem, H2_TAP_RESET_FRAME_BYTES);
  if (state.render_buffer == NULL) {
    tap_reset_deinit(&state);
    return H2_PAL_ERR_NO_MEMORY;
  }

  lv_init();
  state.lvgl_initialized = 1;
  state.display = lv_display_create(H2_TAP_RESET_WIDTH, H2_TAP_RESET_HEIGHT);
  if (state.display == NULL) {
    tap_reset_deinit(&state);
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_display_set_default(state.display);
  lv_display_set_user_data(state.display, &state);
  lv_tick_set_cb(tap_reset_tick_get);
  lv_display_set_color_format(state.display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(state.display, tap_reset_flush);
  lv_display_set_buffers(state.display, state.render_buffer, NULL,
                         H2_TAP_RESET_FRAME_BYTES, LV_DISPLAY_RENDER_MODE_FULL);

  state.pointer = lv_indev_create();
  if (state.pointer == NULL) {
    tap_reset_deinit(&state);
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_indev_set_type(state.pointer, LV_INDEV_TYPE_POINTER);
  lv_indev_set_user_data(state.pointer, &state);
  lv_indev_set_read_cb(state.pointer, tap_reset_pointer_read);
  if (!tap_reset_create_ui(&state)) {
    tap_reset_deinit(&state);
    return H2_PAL_ERR_NO_MEMORY;
  }

  while (!config->should_stop(config->stop_user)) {
    (void)lv_timer_handler();
    result = h2_pal_time_sleep_ms(runtime->time, 16u);
    if (result != H2_PAL_OK) {
      tap_reset_deinit(&state);
      return result;
    }
  }
  tap_reset_deinit(&state);
  return H2_PAL_OK;
}
