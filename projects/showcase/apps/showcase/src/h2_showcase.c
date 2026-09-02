#include "h2_showcase.h"
#include "h2_showcase_task_names.h"

#include "h2_gizclaw.h"
#include "h2_gizclaw_task_names.h"
#include "h2_lvgl_platform.h"
#include "h2_lvgl_touch.h"
#include "h2_mp4_decoder.h"
#include "h2_showcase_state.h"
#include "include/lvgl/font/lv_tiny_ttf.h"
#include "lvgl.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_SHOWCASE_WIDTH 1024
#define H2_SHOWCASE_HEIGHT 600
#define H2_SHOWCASE_FRAME_BYTES                                                \
  ((size_t)H2_SHOWCASE_WIDTH * H2_SHOWCASE_HEIGHT * sizeof(uint16_t))
#define H2_SHOWCASE_FRAME_TIMEOUT_MS 1u
#define H2_SHOWCASE_AUDIO_SAMPLE_RATE_HZ 16000u
#define H2_SHOWCASE_AUDIO_WRITE_TIMEOUT_MS 100u
#define H2_SHOWCASE_GIZCLAW_POLL_MS 20
#define H2_SHOWCASE_GIZCLAW_DEFAULT_CONNECT_TIMEOUT_MS 45000
#define H2_SHOWCASE_PREF_NAMESPACE "showcase"
#define H2_SHOWCASE_PREF_VIDEO_ID "video_id"
#define H2_SHOWCASE_PREF_CHARACTER_ID "character_id"

typedef enum h2_showcase_gizclaw_status {
  H2_SHOWCASE_GIZCLAW_DISABLED = 0,
  H2_SHOWCASE_GIZCLAW_CONNECTING,
  H2_SHOWCASE_GIZCLAW_CONNECTED,
  H2_SHOWCASE_GIZCLAW_FAILED,
} h2_showcase_gizclaw_status_t;

typedef struct h2_showcase_gizclaw_state {
  struct h2_showcase_app *app;
  h2_pal_task_t *task;
  h2_gizclaw_service_t *service;
  atomic_bool stop;
  atomic_int status;
} h2_showcase_gizclaw_state_t;

typedef struct h2_showcase_audio_state {
  const uint8_t *pcm_data;
  size_t pcm_size;
  h2_audio_info_t info;
  h2_pal_audio_track_t *track;
  h2_pal_task_t *task;
  uint8_t *frame_buffer;
  size_t frame_bytes;
  size_t offset;
  atomic_bool stop;
  atomic_bool failed;
  int speaker_started;
} h2_showcase_audio_state_t;

typedef struct h2_showcase_app {
  h2_runtime_t *runtime;
  const h2_showcase_config_t *config;
  h2_showcase_state_t state;
  uint16_t *background_buffer;
  uint16_t *render_buffer;
  lv_display_t *display;
  lv_indev_t *pointer;
  h2_lvgl_touch_t *touch_adapter;
  lv_font_t *font;
  lv_obj_t *background;
  lv_obj_t *console;
  lv_obj_t *list;
  lv_obj_t *video_tab;
  lv_obj_t *character_tab;
  lv_obj_t *page_title;
  lv_obj_t *current_video_label;
  lv_obj_t *current_character_label;
  lv_obj_t *status_label;
  lv_obj_t *conversation;
  lv_obj_t *conversation_bars[3];
  int active_tab;
  int active_video;
  int selected_video;
  int pending_video;
  int active_character;
  int selected_character;
  uint32_t animation_step;
  uint64_t lvgl_last_tick_ms;
  int lvgl_tick_started;
  h2_mp4_decoder_t *decoder;
  h2_pal_fs_file_t *decoder_file;
  uint64_t decoder_file_position;
  uint64_t decoder_base_clock_ms;
  int64_t decoder_base_pts_us;
  int decoder_clock_ready;
  int display_opened;
  int platform_initialized;
  int lvgl_initialized;
  h2_showcase_audio_state_t *audio;
  h2_showcase_gizclaw_state_t *gizclaw;
} h2_showcase_app_t;

static bool gizclaw_cancel_requested(void *user) {
  h2_showcase_gizclaw_state_t *state = user;
  return state == NULL ||
         atomic_load_explicit(&state->stop, memory_order_acquire);
}

static void gizclaw_log(h2_showcase_app_t *app, h2_pal_log_level_t level,
                        const char *message) {
  if (app->runtime->log != NULL) {
    (void)h2_pal_log_write(app->runtime->log, level, "showcase", message);
  }
}

static void gizclaw_registration_complete(
    void *user, h2_gizclaw_registration_request_t *request) {
  h2_showcase_gizclaw_state_t *state = user;
  h2_showcase_app_t *app = state->app;
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_registration_request_operation_result(request);
  const h2_gizclaw_registration_result_t *registration =
      h2_gizclaw_registration_request_response(request);
  if (result != NULL && result->result == H2_PAL_OK && registration != NULL &&
      strcmp(registration->runtime_profile_name,
             app->config->gizclaw_runtime_profile_name) == 0) {
    atomic_store_explicit(&state->status, H2_SHOWCASE_GIZCLAW_CONNECTED,
                          memory_order_release);
    gizclaw_log(app, H2_PAL_LOG_INFO,
                "H2_SHOWCASE_GIZCLAW connected profile=showcase");
  } else {
    atomic_store_explicit(&state->status, H2_SHOWCASE_GIZCLAW_FAILED,
                          memory_order_release);
    gizclaw_log(app, H2_PAL_LOG_ERROR, "H2_SHOWCASE_GIZCLAW failed");
    atomic_store_explicit(&state->stop, true, memory_order_release);
  }
  h2_gizclaw_registration_request_release(request);
}

static void gizclaw_terminal(void *user, h2_pal_result_t result) {
  (void)result;
  h2_showcase_gizclaw_state_t *state = user;
  if (!gizclaw_cancel_requested(state)) {
    atomic_store_explicit(&state->status, H2_SHOWCASE_GIZCLAW_FAILED,
                          memory_order_release);
    gizclaw_log(state->app, H2_PAL_LOG_ERROR, "H2_SHOWCASE_GIZCLAW failed");
    atomic_store_explicit(&state->stop, true, memory_order_release);
  }
}

static void gizclaw_task_entry(void *context) {
  h2_showcase_gizclaw_state_t *state = context;
  h2_showcase_app_t *app = state->app;
  const h2_showcase_config_t *showcase = app->config;
  const int connect_timeout_ms =
      showcase->gizclaw_connect_timeout_ms == 0u
          ? H2_SHOWCASE_GIZCLAW_DEFAULT_CONNECT_TIMEOUT_MS
          : (int)showcase->gizclaw_connect_timeout_ms;
  const h2_gizclaw_config_t config = {
      .server_endpoint = {.data = showcase->gizclaw_server_endpoint,
                          .len = strlen(showcase->gizclaw_server_endpoint)},
      .private_key = {.data = showcase->gizclaw_private_key,
                      .len = strlen(showcase->gizclaw_private_key)},
      .cipher_mode = H2_GIZCLAW_CIPHER_CHACHA20_POLY1305,
      .connect_timeout_ms = connect_timeout_ms,
      .allocator = app->runtime->mem,
      .http = app->runtime->http,
      .webrtc = app->runtime->webrtc,
      .webrtc_media_track = app->runtime->webrtc_media_track,
      .crypto = app->runtime->crypto,
      .time = app->runtime->time,
      .log = app->runtime->log,
      .cancel_requested = gizclaw_cancel_requested,
      .cancel_user = state,
  };
  const h2_gizclaw_service_config_t service_config = {
      .client_config = &config,
      .task = app->runtime->task,
      .queue = app->runtime->queue,
      .sync = app->runtime->sync,
      .net_task_options = {.name = h2_gizclaw_net_task_name,
                           .min_stack_size = 16384u},
      .operation_capacity = 4u,
      .client_poll_timeout_ms = H2_SHOWCASE_GIZCLAW_POLL_MS,
      .terminal = gizclaw_terminal,
      .terminal_user = state,
  };
  h2_pal_result_t result =
      h2_gizclaw_service_init(&service_config, &state->service);
  if (result == H2_PAL_OK) {
    result = h2_gizclaw_service_start(state->service);
  }
  h2_gizclaw_registration_request_t *registration = NULL;
  if (result == H2_PAL_OK) {
    result = h2_gizclaw_service_register_async(
        state->service, 1u, showcase->gizclaw_registration_token, 30000u,
        gizclaw_registration_complete, state, &registration);
  }
  if (result == H2_PAL_OK) {
    while (!gizclaw_cancel_requested(state)) {
      size_t dispatched = 0u;
      result = h2_gizclaw_service_poll(state->service, 8u, &dispatched);
      if (result != H2_PAL_OK)
        break;
      if (h2_pal_time_sleep_ms(app->runtime->time,
                               H2_SHOWCASE_GIZCLAW_POLL_MS) != H2_PAL_OK)
        break;
    }
  }
  if (state->service != NULL) {
    const h2_pal_result_t stop_result = h2_gizclaw_service_stop(state->service);
    if (result == H2_PAL_OK)
      result = stop_result;
    for (;;) {
      size_t dispatched = 0u;
      const h2_pal_result_t dispatch_result =
          h2_gizclaw_service_poll(state->service, 8u, &dispatched);
      if (result == H2_PAL_OK)
        result = dispatch_result;
      if (dispatch_result != H2_PAL_OK || dispatched == 0u)
        break;
    }
    const h2_pal_result_t deinit_result =
        h2_gizclaw_service_deinit(state->service);
    if (result == H2_PAL_OK)
      result = deinit_result;
    state->service = NULL;
  }
  if (!gizclaw_cancel_requested(state) && result != H2_PAL_OK) {
    atomic_store_explicit(&state->status, H2_SHOWCASE_GIZCLAW_FAILED,
                          memory_order_release);
    gizclaw_log(app, H2_PAL_LOG_ERROR, "H2_SHOWCASE_GIZCLAW failed");
  }
}

static h2_pal_result_t gizclaw_init(h2_showcase_app_t *app) {
  const h2_showcase_config_t *config = app->config;
  const int any_config = config->gizclaw_server_endpoint != NULL ||
                         config->gizclaw_private_key != NULL ||
                         config->gizclaw_registration_token != NULL;
  if (!any_config) {
    return H2_PAL_OK;
  }
  if (config->gizclaw_server_endpoint == NULL ||
      config->gizclaw_server_endpoint[0] == '\0' ||
      config->gizclaw_private_key == NULL ||
      config->gizclaw_private_key[0] == '\0' ||
      config->gizclaw_registration_token == NULL ||
      config->gizclaw_registration_token[0] == '\0' ||
      config->gizclaw_runtime_profile_name == NULL ||
      strcmp(config->gizclaw_runtime_profile_name, "showcase") != 0 ||
      config->gizclaw_connect_timeout_ms > (uint32_t)INT_MAX ||
      app->runtime->task == NULL || app->runtime->http == NULL ||
      app->runtime->webrtc == NULL || app->runtime->crypto == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  app->gizclaw = h2_pal_mem_alloc(app->runtime->mem, sizeof(*app->gizclaw));
  if (app->gizclaw == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(app->gizclaw, 0, sizeof(*app->gizclaw));
  app->gizclaw->app = app;
  atomic_init(&app->gizclaw->stop, false);
  atomic_init(&app->gizclaw->status, H2_SHOWCASE_GIZCLAW_CONNECTING);
  const h2_pal_task_options_t options = {
      .name = h2_showcase_gizclaw_task_name,
      .min_stack_size = 16384u,
  };
  const h2_pal_result_t result =
      h2_pal_task_start(app->runtime->task, &options, gizclaw_task_entry,
                        app->gizclaw, &app->gizclaw->task);
  if (result != H2_PAL_OK) {
    h2_pal_mem_free(app->runtime->mem, app->gizclaw);
    app->gizclaw = NULL;
  }
  return result;
}

static h2_pal_result_t gizclaw_deinit(h2_showcase_app_t *app) {
  if (app->gizclaw == NULL) {
    return H2_PAL_OK;
  }
  atomic_store_explicit(&app->gizclaw->stop, true, memory_order_release);
  h2_pal_result_t result = H2_PAL_OK;
  if (app->gizclaw->task != NULL) {
    result = h2_pal_task_join(app->runtime->task, app->gizclaw->task);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  h2_pal_mem_free(app->runtime->mem, app->gizclaw);
  app->gizclaw = NULL;
  return result;
}

static void display_flush(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels) {
  h2_showcase_app_t *app = lv_display_get_user_data(display);
  const h2_display_rect_t rect = {
      .x = area->x1,
      .y = area->y1,
      .width = area->x2 - area->x1 + 1,
      .height = area->y2 - area->y1 + 1,
  };
  const size_t stride = (size_t)rect.width * sizeof(uint16_t);
  if (h2_pal_display_draw_bitmap(app->runtime->display, &rect, pixels, stride,
                                 H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_OK) {
    (void)h2_pal_display_present(app->runtime->display);
  }
  lv_display_flush_ready(display);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data) {
  h2_showcase_app_t *app = lv_indev_get_user_data(indev);
  h2_showcase_pointer_state_t state = {0};
  if (app->config->read_pointer != NULL &&
      app->config->read_pointer(app->config->pointer_user, &state) ==
          H2_PAL_OK) {
    data->point.x = state.x;
    data->point.y = state.y;
    data->state =
        state.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void style_button(lv_obj_t *button, uint32_t color) {
  lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(button, 12, LV_PART_MAIN);
}

static lv_obj_t *add_label(h2_showcase_app_t *app, lv_obj_t *parent,
                           const char *text) {
  lv_obj_t *label = lv_label_create(parent);
  if (label != NULL) {
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, app->font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffffu), LV_PART_MAIN);
  }
  return label;
}

static int find_video(const h2_showcase_config_t *config, const char *id) {
  for (size_t index = 0; index < config->video_count; ++index) {
    if (strcmp(config->videos[index].id, id) == 0) {
      return (int)index;
    }
  }
  return -1;
}

static int find_character(const h2_showcase_config_t *config, const char *id) {
  for (size_t index = 0; index < config->character_count; ++index) {
    if (strcmp(config->characters[index].id, id) == 0) {
      return (int)index;
    }
  }
  return -1;
}

static void load_preferences(h2_showcase_app_t *app) {
  if (app->runtime->pref == NULL) {
    return;
  }
  h2_pal_pref_namespace_t *pref = NULL;
  if (h2_pal_pref_open(app->runtime->pref, H2_SHOWCASE_PREF_NAMESPACE,
                       H2_PAL_PREF_OPEN_READ_ONLY, &pref) != H2_PAL_OK ||
      pref == NULL) {
    return;
  }
  char *value = NULL;
  if (pref->get_string != NULL &&
      pref->get_string(pref, app->runtime->mem, H2_SHOWCASE_PREF_VIDEO_ID,
                       &value) == H2_PAL_OK &&
      value != NULL) {
    const int selected = find_video(app->config, value);
    if (selected >= 0) {
      app->active_video = selected;
      app->selected_video = selected;
    }
  }
  if (value != NULL) {
    h2_pal_mem_free(app->runtime->mem, value);
    value = NULL;
  }
  if (pref->get_string != NULL &&
      pref->get_string(pref, app->runtime->mem, H2_SHOWCASE_PREF_CHARACTER_ID,
                       &value) == H2_PAL_OK &&
      value != NULL) {
    const int selected = find_character(app->config, value);
    if (selected >= 0) {
      app->active_character = selected;
      app->selected_character = selected;
    }
  }
  if (value != NULL) {
    h2_pal_mem_free(app->runtime->mem, value);
  }
  if (pref->close != NULL) {
    (void)pref->close(pref);
  }
}

static h2_pal_result_t save_preference(h2_showcase_app_t *app, const char *key,
                                       const char *value) {
  if (app->runtime->pref == NULL) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  h2_pal_pref_namespace_t *pref = NULL;
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_pref_open(
      app->runtime->pref, H2_SHOWCASE_PREF_NAMESPACE,
      H2_PAL_PREF_OPEN_READ_WRITE, &pref);
  if (result != H2_PAL_OK || pref == NULL || pref->set_string == NULL ||
      pref->commit == NULL || pref->close == NULL) {
    if (pref != NULL && pref->close != NULL) {
      (void)pref->close(pref);
    }
    return result == H2_PAL_OK ? H2_PAL_ERR_UNAVAILABLE : result;
  }
  result = (h2_pal_result_t)pref->set_string(pref, key, value);
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)pref->commit(pref);
  }
  const h2_pal_result_t close_result = (h2_pal_result_t)pref->close(pref);
  return result == H2_PAL_OK ? close_result : result;
}

static void set_status(h2_showcase_app_t *app, const char *text,
                       uint32_t color) {
  lv_label_set_text(app->status_label, text);
  lv_obj_set_style_text_color(app->status_label, lv_color_hex(color),
                              LV_PART_MAIN);
}

static void refresh_current_labels(h2_showcase_app_t *app) {
  char text[192];
  (void)snprintf(text, sizeof(text), "当前视频\n%s",
                 app->config->videos[app->active_video].display_name);
  lv_label_set_text(app->current_video_label, text);
  (void)snprintf(text, sizeof(text), "当前角色\n%s",
                 app->config->characters[app->active_character].display_name);
  lv_label_set_text(app->current_character_label, text);
}

static void refresh_list(h2_showcase_app_t *app);

static void refresh_selection_styles(h2_showcase_app_t *app) {
  const int selected =
      app->active_tab == 0 ? app->selected_video : app->selected_character;
  const uint32_t count = lv_obj_get_child_count(app->list);
  for (uint32_t index = 0; index < count; ++index) {
    style_button(lv_obj_get_child(app->list, (int32_t)index),
                 (int)index == selected ? 0x315c9b : 0x202735);
  }
}

static void list_clicked(lv_event_t *event) {
  h2_showcase_app_t *app = lv_event_get_user_data(event);
  lv_obj_t *target = lv_event_get_target_obj(event);
  const uint32_t count = lv_obj_get_child_count(app->list);
  for (uint32_t index = 0; index < count; ++index) {
    if (lv_obj_get_child(app->list, (int32_t)index) != target) {
      continue;
    }
    if (app->active_tab == 0) {
      app->selected_video = (int)index;
    } else {
      app->selected_character = (int)index;
    }
    refresh_selection_styles(app);
    return;
  }
}

static void refresh_list(h2_showcase_app_t *app) {
  lv_obj_clean(app->list);
  const size_t count = app->active_tab == 0 ? app->config->video_count
                                            : app->config->character_count;
  const int selected =
      app->active_tab == 0 ? app->selected_video : app->selected_character;
  for (size_t index = 0; index < count; ++index) {
    lv_obj_t *button = lv_button_create(app->list);
    if (button == NULL) {
      return;
    }
    lv_obj_set_width(button, lv_pct(100));
    style_button(button, (int)index == selected ? 0x315c9b : 0x202735);
    lv_obj_add_event_cb(button, list_clicked, LV_EVENT_CLICKED, app);
    const char *name = app->active_tab == 0
                           ? app->config->videos[index].display_name
                           : app->config->characters[index].display_name;
    lv_obj_t *label = add_label(app, button, name);
    if (label != NULL) {
      lv_obj_center(label);
    }
  }
}

static void tab_clicked(lv_event_t *event) {
  h2_showcase_app_t *app = lv_event_get_user_data(event);
  app->active_tab = lv_event_get_target_obj(event) == app->character_tab;
  style_button(app->video_tab, app->active_tab == 0 ? 0x315c9b : 0x202735);
  style_button(app->character_tab, app->active_tab == 1 ? 0x315c9b : 0x202735);
  lv_label_set_text(app->page_title,
                    app->active_tab == 0 ? "选择视频" : "选择对话角色");
  set_status(app, "", 0xffffffu);
  refresh_list(app);
}

static void close_console(lv_event_t *event) {
  h2_showcase_app_t *app = lv_event_get_user_data(event);
  app->selected_video = app->active_video;
  app->selected_character = app->active_character;
  app->pending_video = -1;
  set_status(app, "", 0xffffffu);
  h2_showcase_state_close_console(&app->state);
  lv_obj_set_hidden(app->console, true);
}

static void confirm_selection(lv_event_t *event) {
  h2_showcase_app_t *app = lv_event_get_user_data(event);
  if (app->active_tab == 0) {
    if (app->selected_video == app->active_video) {
      const h2_pal_result_t result =
          save_preference(app, H2_SHOWCASE_PREF_VIDEO_ID,
                          app->config->videos[app->active_video].id);
      set_status(
          app, result == H2_PAL_OK ? "当前视频已保存" : "保存视频失败，请重试",
          result == H2_PAL_OK ? 0x91d5a9u : 0xff8585u);
    } else {
      app->pending_video = app->selected_video;
      set_status(app, "正在切换视频…", 0xf6c76bu);
    }
    return;
  }
  const h2_showcase_character_entry_t *character =
      &app->config->characters[app->selected_character];
  const h2_pal_result_t result =
      save_preference(app, H2_SHOWCASE_PREF_CHARACTER_ID, character->id);
  if (result != H2_PAL_OK) {
    set_status(app, "保存角色失败，请重试", 0xff8585u);
    return;
  }
  app->active_character = app->selected_character;
  refresh_current_labels(app);
  set_status(app, "角色已保存", 0x91d5a9u);
}

static int create_console(h2_showcase_app_t *app, lv_obj_t *screen) {
  app->console = lv_obj_create(screen);
  if (app->console == NULL) {
    return 0;
  }
  lv_obj_set_size(app->console, H2_SHOWCASE_WIDTH, H2_SHOWCASE_HEIGHT);
  lv_obj_center(app->console);
  lv_obj_set_scrollable(app->console, false);
  lv_obj_set_style_bg_color(app->console, lv_color_hex(0x0b1019u),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(app->console, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(app->console, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(app->console, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(app->console, 28, LV_PART_MAIN);

  lv_obj_t *sidebar = lv_obj_create(app->console);
  if (sidebar == NULL) {
    return 0;
  }
  lv_obj_set_size(sidebar, 250, 544);
  lv_obj_align(sidebar, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_opa(sidebar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(sidebar, 0, LV_PART_MAIN);

  app->video_tab = lv_button_create(sidebar);
  app->character_tab = lv_button_create(sidebar);
  if (app->video_tab == NULL || app->character_tab == NULL) {
    return 0;
  }
  lv_obj_set_width(app->video_tab, lv_pct(100));
  lv_obj_set_width(app->character_tab, lv_pct(100));
  style_button(app->video_tab, 0x315c9b);
  style_button(app->character_tab, 0x202735);
  lv_obj_add_event_cb(app->video_tab, tab_clicked, LV_EVENT_CLICKED, app);
  lv_obj_add_event_cb(app->character_tab, tab_clicked, LV_EVENT_CLICKED, app);
  lv_obj_t *video_label = add_label(app, app->video_tab, "视频");
  lv_obj_t *character_label = add_label(app, app->character_tab, "对话角色");
  if (video_label == NULL || character_label == NULL) {
    return 0;
  }
  lv_obj_center(video_label);
  lv_obj_center(character_label);

  lv_obj_t *sidebar_spacer = lv_obj_create(sidebar);
  if (sidebar_spacer == NULL) {
    return 0;
  }
  lv_obj_set_width(sidebar_spacer, lv_pct(100));
  lv_obj_set_flex_grow(sidebar_spacer, 1);
  lv_obj_set_style_bg_opa(sidebar_spacer, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(sidebar_spacer, 0, LV_PART_MAIN);

  app->current_video_label = add_label(app, sidebar, "");
  app->current_character_label = add_label(app, sidebar, "");
  if (app->current_video_label == NULL ||
      app->current_character_label == NULL) {
    return 0;
  }
  lv_obj_set_style_text_color(app->current_video_label, lv_color_hex(0xaeb8c8u),
                              LV_PART_MAIN);
  lv_obj_set_style_text_color(app->current_character_label,
                              lv_color_hex(0xaeb8c8u), LV_PART_MAIN);
  refresh_current_labels(app);

  app->page_title = add_label(app, app->console, "选择视频");
  if (app->page_title == NULL) {
    return 0;
  }
  lv_obj_align(app->page_title, LV_ALIGN_TOP_LEFT, 286, 4);

  app->list = lv_obj_create(app->console);
  if (app->list == NULL) {
    return 0;
  }
  lv_obj_set_size(app->list, 680, 426);
  lv_obj_align(app->list, LV_ALIGN_TOP_RIGHT, 0, 54);
  lv_obj_set_flex_flow(app->list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_opa(app->list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(app->list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(app->list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(app->list, 10, LV_PART_MAIN);
  refresh_list(app);

  app->status_label = add_label(app, app->console, "");
  if (app->status_label == NULL) {
    return 0;
  }
  lv_obj_align(app->status_label, LV_ALIGN_BOTTOM_LEFT, 286, -15);

  lv_obj_t *close = lv_button_create(app->console);
  lv_obj_t *confirm = lv_button_create(app->console);
  if (close == NULL || confirm == NULL) {
    return 0;
  }
  lv_obj_set_size(close, 130, 52);
  lv_obj_set_size(confirm, 130, 52);
  lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -150, 0);
  lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  style_button(close, 0x303746);
  style_button(confirm, 0x315c9b);
  lv_obj_add_event_cb(close, close_console, LV_EVENT_CLICKED, app);
  lv_obj_add_event_cb(confirm, confirm_selection, LV_EVENT_CLICKED, app);
  lv_obj_t *close_label = add_label(app, close, "关闭");
  lv_obj_t *confirm_label = add_label(app, confirm, "确认");
  if (close_label == NULL || confirm_label == NULL) {
    return 0;
  }
  lv_obj_center(close_label);
  lv_obj_center(confirm_label);
  lv_obj_set_hidden(app->console, true);
  return 1;
}

static int create_conversation_effect(h2_showcase_app_t *app,
                                      lv_obj_t *screen) {
  app->conversation = lv_obj_create(screen);
  if (app->conversation == NULL) {
    return 0;
  }
  lv_obj_set_size(app->conversation, 126, 62);
  lv_obj_align(app->conversation, LV_ALIGN_TOP_RIGHT, -24, 24);
  lv_obj_set_flex_flow(app->conversation, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(app->conversation, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_bg_color(app->conversation, lv_color_hex(0x101827u),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(app->conversation, LV_OPA_80, LV_PART_MAIN);
  lv_obj_set_style_border_width(app->conversation, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(app->conversation, 31, LV_PART_MAIN);
  for (size_t index = 0; index < 3u; ++index) {
    app->conversation_bars[index] = lv_obj_create(app->conversation);
    if (app->conversation_bars[index] == NULL) {
      return 0;
    }
    lv_obj_set_size(app->conversation_bars[index], 12, 18 + (int32_t)index * 8);
    lv_obj_set_style_bg_color(app->conversation_bars[index],
                              lv_color_hex(0x7db7ffu), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(app->conversation_bars[index], LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_border_width(app->conversation_bars[index], 0,
                                  LV_PART_MAIN);
    lv_obj_set_style_radius(app->conversation_bars[index], 6, LV_PART_MAIN);
  }
  lv_obj_set_hidden(app->conversation, true);
  return 1;
}

static h2_pal_result_t ui_init(h2_showcase_app_t *app) {
  h2_display_info_t info = {0};
  if (h2_pal_display_open(app->runtime->display) != H2_DISPLAY_OK) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  app->display_opened = 1;
  if (h2_pal_display_get_info(app->runtime->display, &info) != H2_DISPLAY_OK ||
      info.width != H2_SHOWCASE_WIDTH || info.height != H2_SHOWCASE_HEIGHT) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  app->background_buffer =
      h2_pal_mem_alloc(app->runtime->mem, H2_SHOWCASE_FRAME_BYTES);
  app->render_buffer =
      h2_pal_mem_alloc(app->runtime->mem, H2_SHOWCASE_FRAME_BYTES);
  if (app->background_buffer == NULL || app->render_buffer == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(app->background_buffer, 0, H2_SHOWCASE_FRAME_BYTES);
  const h2_lvgl_platform_config_t platform = {
      .allocator = app->runtime->mem,
      .task_api = app->runtime->task,
      .sync_api = app->runtime->sync,
      .queue_api = app->runtime->queue,
      .time_api = app->runtime->time,
  };
  if (h2_lvgl_platform_init(&platform) != H2_PAL_OK) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  app->platform_initialized = 1;
  lv_init();
  app->lvgl_initialized = 1;
  app->display = lv_display_create(H2_SHOWCASE_WIDTH, H2_SHOWCASE_HEIGHT);
  app->font =
      lv_tiny_ttf_create_data_ex(app->config->font_data, app->config->font_size,
                                 20, LV_FONT_KERNING_NONE, 128u);
  if (app->display == NULL || app->font == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_display_set_user_data(app->display, app);
  lv_display_set_color_format(app->display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(app->display, display_flush);
  lv_display_set_buffers(app->display, app->render_buffer, NULL,
                         H2_SHOWCASE_FRAME_BYTES, LV_DISPLAY_RENDER_MODE_FULL);

  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_scrollable(screen, false);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000u), LV_PART_MAIN);
  app->background = lv_canvas_create(screen);
  if (app->background == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_canvas_set_buffer(app->background, app->background_buffer,
                       H2_SHOWCASE_WIDTH, H2_SHOWCASE_HEIGHT,
                       LV_COLOR_FORMAT_RGB565);
  lv_obj_center(app->background);
  if (!create_conversation_effect(app, screen) ||
      !create_console(app, screen)) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (app->config->read_pointer != NULL) {
    app->pointer = lv_indev_create();
    if (app->pointer == NULL) {
      return H2_PAL_ERR_NO_MEMORY;
    }
    lv_indev_set_type(app->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(app->pointer, app->display);
    lv_indev_set_user_data(app->pointer, app);
    lv_indev_set_read_cb(app->pointer, pointer_read);
  } else {
    const h2_lvgl_touch_config_t touch_config = {
        .touch = app->runtime->touch,
        .allocator = app->runtime->mem,
        .display = app->display,
    };
    h2_pal_result_t result =
        h2_lvgl_touch_create(&touch_config, &app->touch_adapter);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  return H2_PAL_OK;
}

static void ui_deinit(h2_showcase_app_t *app) {
  if (app->touch_adapter != NULL) {
    h2_lvgl_touch_destroy(app->touch_adapter);
    app->touch_adapter = NULL;
  }
  if (app->pointer != NULL) {
    lv_indev_delete(app->pointer);
    app->pointer = NULL;
  }
  if (app->display != NULL) {
    lv_display_delete(app->display);
  }
  if (app->font != NULL) {
    lv_tiny_ttf_destroy(app->font);
  }
  if (app->lvgl_initialized) {
    lv_deinit();
  }
  if (app->platform_initialized) {
    h2_lvgl_platform_deinit();
  }
  if (app->render_buffer != NULL) {
    h2_pal_mem_free(app->runtime->mem, app->render_buffer);
  }
  if (app->background_buffer != NULL) {
    h2_pal_mem_free(app->runtime->mem, app->background_buffer);
  }
  if (app->display_opened) {
    (void)h2_pal_display_close(app->runtime->display);
  }
}

static void audio_task_entry(void *context) {
  h2_showcase_audio_state_t *audio = context;
  while (!atomic_load_explicit(&audio->stop, memory_order_acquire)) {
    size_t copied = 0u;
    while (copied < audio->frame_bytes) {
      const size_t available = audio->pcm_size - audio->offset;
      const size_t remaining = audio->frame_bytes - copied;
      const size_t count = available < remaining ? available : remaining;
      memcpy(audio->frame_buffer + copied, audio->pcm_data + audio->offset,
             count);
      copied += count;
      audio->offset += count;
      if (audio->offset == audio->pcm_size) {
        audio->offset = 0u;
      }
    }
    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        audio->frame_buffer, audio->frame_bytes, audio->info.playback_format);
    frame.bytes = audio->frame_bytes;
    for (;;) {
      const int result = h2_pal_audio_track_write(
          audio->track, &frame, H2_SHOWCASE_AUDIO_WRITE_TIMEOUT_MS);
      if (result == H2_AUDIO_OK) {
        break;
      }
      if (atomic_load_explicit(&audio->stop, memory_order_acquire)) {
        return;
      }
      if (result != H2_AUDIO_ERR_WOULD_BLOCK && result != H2_PAL_ERR_TIMEOUT) {
        atomic_store_explicit(&audio->failed, true, memory_order_release);
        return;
      }
    }
  }
}

static h2_pal_result_t audio_init(h2_showcase_app_t *app) {
  const h2_showcase_video_entry_t *video =
      &app->config->videos[app->active_video];
  if (video->audio_pcm_data == NULL || video->audio_pcm_size == 0u ||
      (video->audio_pcm_size % sizeof(int16_t)) != 0u ||
      app->runtime->audio == NULL || app->runtime->task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  app->audio = h2_pal_mem_alloc(app->runtime->mem, sizeof(*app->audio));
  if (app->audio == NULL) {
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  memset(app->audio, 0, sizeof(*app->audio));
  app->audio->pcm_data = video->audio_pcm_data;
  app->audio->pcm_size = video->audio_pcm_size;
  atomic_init(&app->audio->stop, false);
  atomic_init(&app->audio->failed, false);
  int result = h2_pal_audio_get_info(app->runtime->audio, &app->audio->info);
  const h2_audio_pcm_format_t *format = &app->audio->info.playback_format;
  if (result != H2_AUDIO_OK || !app->audio->info.available ||
      !app->audio->info.playback_supported ||
      format->sample_rate_hz != H2_SHOWCASE_AUDIO_SAMPLE_RATE_HZ ||
      format->channels != 1u ||
      format->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      format->frame_samples_per_channel == 0u) {
    return result == H2_AUDIO_OK ? H2_AUDIO_ERR_UNSUPPORTED : result;
  }
  app->audio->frame_bytes =
      (size_t)format->frame_samples_per_channel * sizeof(int16_t);
  app->audio->frame_buffer =
      h2_pal_mem_alloc(app->runtime->mem, app->audio->frame_bytes);
  if (app->audio->frame_buffer == NULL) {
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  result = h2_pal_audio_start_speaker(app->runtime->audio);
  if (result != H2_AUDIO_OK) {
    return result;
  }
  app->audio->speaker_started = 1;
  (void)h2_pal_audio_set_speaker_volume_percent(app->runtime->audio, 70u);
  const h2_audio_track_config_t track_config = {
      .name = "showcase-media",
      .format = *format,
      .volume_factor_milli = 1000u,
      .buffer_frames = 8u,
  };
  result = h2_pal_audio_create_track(app->runtime->audio, &track_config,
                                     &app->audio->track);
  if (result != H2_AUDIO_OK) {
    return result;
  }
  const h2_pal_task_options_t task_options = {
      .name = h2_showcase_audio_task_name,
      .min_stack_size = 8192u,
  };
  result = h2_pal_task_start(app->runtime->task, &task_options,
                             audio_task_entry, app->audio, &app->audio->task);
  if (result == H2_PAL_OK && app->runtime->log != NULL) {
    (void)h2_pal_log_write(app->runtime->log, H2_PAL_LOG_INFO, "showcase",
                           "H2_SHOWCASE_AUDIO started looping=1");
  }
  return result;
}

static h2_pal_result_t audio_deinit(h2_showcase_app_t *app) {
  if (app->audio == NULL) {
    return H2_PAL_OK;
  }
  h2_showcase_audio_state_t *audio = app->audio;
  h2_pal_result_t result = H2_PAL_OK;
  atomic_store_explicit(&audio->stop, true, memory_order_release);
  if (audio->task != NULL) {
    result = h2_pal_task_join(app->runtime->task, audio->task);
    if (result != H2_PAL_OK) {
      return result;
    }
    audio->task = NULL;
  }
  if (audio->track != NULL) {
    const h2_pal_result_t close_result =
        (h2_pal_result_t)h2_pal_audio_track_close(audio->track);
    if (result == H2_PAL_OK) {
      result = close_result;
    }
    audio->track = NULL;
  }
  if (audio->speaker_started) {
    const h2_pal_result_t stop_result =
        (h2_pal_result_t)h2_pal_audio_stop_speaker(app->runtime->audio);
    if (result == H2_PAL_OK) {
      result = stop_result;
    }
    audio->speaker_started = 0;
  }
  if (audio->frame_buffer != NULL) {
    h2_pal_mem_free(app->runtime->mem, audio->frame_buffer);
  }
  h2_pal_mem_free(app->runtime->mem, audio);
  app->audio = NULL;
  return result;
}

static h2_pal_result_t decoder_read_at(void *user, uint64_t offset,
                                       void *buffer, size_t capacity,
                                       size_t *out_read) {
  h2_showcase_app_t *app = user;
  if (app == NULL || app->decoder_file == NULL || out_read == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (offset != app->decoder_file_position) {
    h2_pal_result_t result =
        h2_pal_fs_seek(app->runtime->fs, app->decoder_file, offset);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  h2_pal_result_t result = h2_pal_fs_read(app->runtime->fs, app->decoder_file,
                                          buffer, capacity, out_read);
  if (result == H2_PAL_OK) {
    if (*out_read > UINT64_MAX - offset) {
      return H2_PAL_ERR_FORMAT;
    }
    app->decoder_file_position = offset + *out_read;
  }
  return result;
}

static h2_pal_result_t decoder_deinit(h2_showcase_app_t *app) {
  h2_pal_result_t result = H2_PAL_OK;
  if (app->decoder != NULL) {
    result = h2_mp4_decoder_close(app->decoder);
    app->decoder = NULL;
  }
  if (app->decoder_file != NULL) {
    const h2_pal_result_t close_result =
        h2_pal_fs_close(app->runtime->fs, app->decoder_file);
    if (result == H2_PAL_OK) {
      result = close_result;
    }
    app->decoder_file = NULL;
  }
  app->decoder_file_position = 0u;
  return result;
}

static h2_pal_result_t decoder_init(h2_showcase_app_t *app) {
  const char *path = app->config->videos[app->active_video].path;
  h2_pal_fs_stat_t stat = {0};
  h2_pal_result_t result = h2_pal_fs_stat(app->runtime->fs, path, &stat);
  if (result != H2_PAL_OK || stat.is_dir || stat.size == 0u) {
    return result != H2_PAL_OK ? result : H2_PAL_ERR_INVALID_ARG;
  }
  result = h2_pal_fs_open(app->runtime->fs, path, H2_PAL_FS_OPEN_READ,
                          &app->decoder_file);
  if (result != H2_PAL_OK) {
    return result;
  }
  const h2_mp4_decoder_config_t config = {
      .allocator = app->runtime->mem,
      .source =
          {
              .user = app,
              .size = stat.size,
              .read_at = decoder_read_at,
          },
      .video_decoder = *app->runtime->video_decoder,
      .video_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
      .require_video = 1,
  };
  result = h2_mp4_decoder_open(&config, &app->decoder);
  if (result != H2_PAL_OK) {
    (void)decoder_deinit(app);
    return result;
  }
  h2_mp4_decoder_info_t info = {0};
  result = h2_mp4_decoder_get_info(app->decoder, &info);
  if (result != H2_PAL_OK || !info.has_video ||
      info.width != H2_SHOWCASE_WIDTH || info.height != H2_SHOWCASE_HEIGHT) {
    if (result == H2_PAL_OK) {
      result = H2_PAL_ERR_FORMAT;
    }
    (void)decoder_deinit(app);
  }
  return result;
}

static h2_pal_result_t apply_pending_video(h2_showcase_app_t *app) {
  if (app->pending_video < 0) {
    return H2_PAL_OK;
  }
  const int requested = app->pending_video;
  app->pending_video = -1;
  if ((size_t)requested >= app->config->video_count) {
    set_status(app, "视频已失效，请重新选择", 0xff8585u);
    return H2_PAL_OK;
  }
  if (requested == app->active_video) {
    set_status(app, "当前视频已生效", 0x91d5a9u);
    return H2_PAL_OK;
  }

  const int previous = app->active_video;
  (void)decoder_deinit(app);
  h2_pal_result_t result = audio_deinit(app);
  if (result != H2_PAL_OK) {
    set_status(app, "停止原视频失败", 0xff8585u);
    return result;
  }
  app->active_video = requested;
  app->decoder_clock_ready = 0;
  result = decoder_init(app);
  if (result == H2_PAL_OK) {
    result = audio_init(app);
  }
  if (result == H2_PAL_OK) {
    result = save_preference(app, H2_SHOWCASE_PREF_VIDEO_ID,
                             app->config->videos[requested].id);
  }
  if (result == H2_PAL_OK) {
    refresh_current_labels(app);
    set_status(app, "视频和声音已切换", 0x91d5a9u);
    return H2_PAL_OK;
  }

  if (app->decoder != NULL) {
    (void)decoder_deinit(app);
  }
  const h2_pal_result_t cleanup_result = audio_deinit(app);
  if (cleanup_result != H2_PAL_OK) {
    return cleanup_result;
  }
  app->active_video = previous;
  app->selected_video = previous;
  app->decoder_clock_ready = 0;
  const h2_pal_result_t decoder_result = decoder_init(app);
  const h2_pal_result_t audio_result =
      decoder_result == H2_PAL_OK ? audio_init(app) : decoder_result;
  refresh_selection_styles(app);
  refresh_current_labels(app);
  set_status(app, "视频切换失败，已恢复原视频", 0xff8585u);
  return audio_result;
}

static int frame_plane_covers_rows(const h2_mp4_decoder_frame_info_t *info) {
  const size_t row_bytes = (size_t)info->width * sizeof(uint16_t);
  const size_t preceding_rows = (size_t)info->height - 1u;
  if (preceding_rows > 0u && info->video_planes[0].stride_bytes >
                                 (SIZE_MAX - row_bytes) / preceding_rows) {
    return 0;
  }
  return info->video_planes[0].bytes >=
         preceding_rows * info->video_planes[0].stride_bytes + row_bytes;
}

static h2_pal_result_t update_video(h2_showcase_app_t *app) {
  h2_mp4_decoder_frame_t *frame = NULL;
  h2_pal_result_t result = h2_mp4_decoder_acquire_frame(
      app->decoder, H2_SHOWCASE_FRAME_TIMEOUT_MS, &frame);
  if (result == H2_PAL_EXIT) {
    result = h2_mp4_decoder_reset(app->decoder);
    if (result == H2_PAL_OK) {
      app->decoder_clock_ready = 0;
    }
    return result;
  }
  if (result == H2_PAL_ERR_TIMEOUT) {
    return H2_PAL_OK;
  }
  if (result != H2_PAL_OK) {
    return result;
  }
  h2_mp4_decoder_frame_info_t info = {0};
  result = h2_mp4_decoder_frame_get_info(app->decoder, frame, &info);
  if (result == H2_PAL_OK &&
      (info.video_format != H2_VIDEO_PIXEL_FORMAT_RGB565 ||
       info.width != H2_SHOWCASE_WIDTH || info.height != H2_SHOWCASE_HEIGHT ||
       info.video_plane_count != 1u || info.video_planes[0].data == NULL ||
       info.video_planes[0].stride_bytes <
           H2_SHOWCASE_WIDTH * sizeof(uint16_t) ||
       !frame_plane_covers_rows(&info))) {
    result = H2_PAL_ERR_FORMAT;
  }
  if (result == H2_PAL_OK && !app->decoder_clock_ready) {
    app->decoder_base_pts_us = info.pts_us;
    result = h2_pal_time_get_monotonic_ms(app->runtime->time,
                                          &app->decoder_base_clock_ms);
    app->decoder_clock_ready = result == H2_PAL_OK;
  } else if (result == H2_PAL_OK && info.pts_us > app->decoder_base_pts_us) {
    uint64_t now_ms = 0u;
    result = h2_pal_time_get_monotonic_ms(app->runtime->time, &now_ms);
    const uint64_t target_ms =
        app->decoder_base_clock_ms +
        (uint64_t)(info.pts_us - app->decoder_base_pts_us) / UINT64_C(1000);
    while (result == H2_PAL_OK && target_ms > now_ms) {
      const uint64_t remaining_ms = target_ms - now_ms;
      result = h2_pal_time_sleep_ms(
          app->runtime->time,
          (uint32_t)(remaining_ms > 8u ? 8u : remaining_ms));
      if (result == H2_PAL_OK) {
        result = h2_pal_time_get_monotonic_ms(app->runtime->time, &now_ms);
      }
    }
  }
  if (result == H2_PAL_OK) {
    const uint8_t *source = info.video_planes[0].data;
    uint8_t *destination = (uint8_t *)app->background_buffer;
    const size_t row_bytes = H2_SHOWCASE_WIDTH * sizeof(uint16_t);
    for (size_t row = 0; row < H2_SHOWCASE_HEIGHT; ++row) {
      memcpy(destination + row * row_bytes,
             source + row * info.video_planes[0].stride_bytes, row_bytes);
    }
    lv_obj_invalidate(app->background);
  }
  const h2_pal_result_t release_result =
      h2_mp4_decoder_release_frame(app->decoder, frame);
  return result == H2_PAL_OK ? release_result : result;
}

static void update_projection(h2_showcase_app_t *app) {
  if (app->state.mode == H2_SHOWCASE_MODE_CONSOLE) {
    lv_obj_set_hidden(app->console, false);
    lv_obj_set_hidden(app->conversation, true);
    return;
  }
  lv_obj_set_hidden(app->console, true);
  if (app->state.mode == H2_SHOWCASE_MODE_CONVERSATION) {
    lv_obj_set_hidden(app->conversation, false);
    for (size_t index = 0; index < 3u; ++index) {
      const uint32_t phase = (app->animation_step + (uint32_t)index) % 5u;
      lv_obj_set_height(app->conversation_bars[index], 18 + (int32_t)phase * 7);
    }
    ++app->animation_step;
  } else {
    lv_obj_set_hidden(app->conversation, true);
  }
}

static void process_runtime_events(h2_showcase_app_t *app) {
  const h2_showcase_mode_t previous_mode = app->state.mode;
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
  while (h2_runtime_poll_event(app->runtime, &event) == H2_PAL_OK) {
    if (event.component_id != H2_SHOWCASE_COMPONENT_ACTION_BUTTON) {
      continue;
    }
    if (event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
        event.payload_size >= sizeof(h2_runtime_button_action_event_t)) {
      const h2_runtime_button_action_event_t *action = event.payload;
      if (action->released_at_ms == 0u &&
          event.timestamp_ms == action->pressed_at_ms) {
        h2_showcase_state_button_down(&app->state, event.timestamp_ms);
      } else if (h2_runtime_button_action_is_released(action)) {
        h2_showcase_state_button_up(&app->state, event.timestamp_ms);
      }
    }
  }
  uint64_t now_ms = 0u;
  if (h2_pal_time_get_monotonic_ms(app->runtime->time, &now_ms) == H2_PAL_OK) {
    h2_showcase_state_tick(&app->state, now_ms);
  }
  if (app->state.mode == H2_SHOWCASE_MODE_CONSOLE &&
      previous_mode != H2_SHOWCASE_MODE_CONSOLE) {
    app->selected_video = app->active_video;
    app->selected_character = app->active_character;
    app->pending_video = -1;
    set_status(app, "", 0xffffffu);
    refresh_list(app);
  }
  if (app->runtime->log != NULL && app->state.mode != previous_mode) {
    const char *message = app->state.mode == H2_SHOWCASE_MODE_CONSOLE
                              ? "H2_SHOWCASE_MODE console"
                          : app->state.mode == H2_SHOWCASE_MODE_CONVERSATION
                              ? "H2_SHOWCASE_MODE conversation"
                              : "H2_SHOWCASE_MODE idle";
    (void)h2_pal_log_write(app->runtime->log, H2_PAL_LOG_INFO, "showcase",
                           message);
  }
}

static h2_pal_result_t update_lvgl(h2_showcase_app_t *app) {
  uint64_t now_ms = 0u;
  h2_pal_result_t result =
      h2_pal_time_get_monotonic_ms(app->runtime->time, &now_ms);
  if (result != H2_PAL_OK) {
    return result;
  }
  uint64_t elapsed_ms = 0u;
  if (app->lvgl_tick_started && now_ms >= app->lvgl_last_tick_ms) {
    elapsed_ms = now_ms - app->lvgl_last_tick_ms;
  }
  if (elapsed_ms > UINT32_MAX) {
    elapsed_ms = UINT32_MAX;
  }
  app->lvgl_tick_started = 1;
  app->lvgl_last_tick_ms = now_ms;
  lv_tick_inc((uint32_t)elapsed_ms);
  (void)lv_timer_handler();
  if (app->touch_adapter != NULL) {
    result = h2_lvgl_touch_last_result(app->touch_adapter);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_showcase_run(h2_runtime_t *runtime,
                                const h2_showcase_config_t *config) {
  if (runtime == NULL || config == NULL || config->videos == NULL ||
      config->video_count == 0u || config->characters == NULL ||
      config->character_count == 0u || config->video_count > (size_t)INT_MAX ||
      config->character_count > (size_t)INT_MAX || config->font_data == NULL ||
      config->font_size == 0u || runtime->display == NULL ||
      runtime->video_decoder == NULL || runtime->fs == NULL ||
      runtime->audio == NULL || runtime->mem == NULL || runtime->time == NULL ||
      runtime->pref == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (size_t index = 0; index < config->video_count; ++index) {
    const h2_showcase_video_entry_t *video = &config->videos[index];
    if (video->id == NULL || video->id[0] == '\0' ||
        video->display_name == NULL || video->display_name[0] == '\0' ||
        video->path == NULL || video->path[0] == '\0' ||
        video->audio_pcm_data == NULL || video->audio_pcm_size == 0u ||
        (video->audio_pcm_size % sizeof(int16_t)) != 0u) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t prior = 0; prior < index; ++prior) {
      if (strcmp(config->videos[prior].id, video->id) == 0) {
        return H2_PAL_ERR_INVALID_ARG;
      }
    }
  }
  for (size_t index = 0; index < config->character_count; ++index) {
    const h2_showcase_character_entry_t *character = &config->characters[index];
    if (character->id == NULL || character->id[0] == '\0' ||
        character->display_name == NULL || character->display_name[0] == '\0') {
      return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t prior = 0; prior < index; ++prior) {
      if (strcmp(config->characters[prior].id, character->id) == 0) {
        return H2_PAL_ERR_INVALID_ARG;
      }
    }
  }
  h2_showcase_app_t app = {
      .runtime = runtime,
      .config = config,
      .selected_video = 0,
      .selected_character = 0,
      .pending_video = -1,
  };
  h2_showcase_state_init(&app.state);
  load_preferences(&app);
  h2_pal_result_t result = ui_init(&app);
  if (result != H2_PAL_OK) {
    ui_deinit(&app);
    return result;
  }
  result = decoder_init(&app);
  if (result != H2_PAL_OK) {
    ui_deinit(&app);
    return result;
  }
  result = audio_init(&app);
  if (result != H2_PAL_OK) {
    (void)decoder_deinit(&app);
    (void)audio_deinit(&app);
    ui_deinit(&app);
    return result;
  }
  result = gizclaw_init(&app);
  if (result != H2_PAL_OK) {
    (void)decoder_deinit(&app);
    (void)audio_deinit(&app);
    ui_deinit(&app);
    return result;
  }
  while (config->should_stop == NULL ||
         !config->should_stop(config->stop_user)) {
    process_runtime_events(&app);
    result = update_video(&app);
    if (result != H2_PAL_OK) {
      break;
    }
    if (app.audio != NULL &&
        atomic_load_explicit(&app.audio->failed, memory_order_acquire)) {
      result = H2_AUDIO_ERR_IO;
      break;
    }
    update_projection(&app);
    result = update_lvgl(&app);
    if (result != H2_PAL_OK) {
      break;
    }
    result = apply_pending_video(&app);
    if (result != H2_PAL_OK) {
      break;
    }
    (void)h2_pal_time_sleep_ms(runtime->time, 8u);
  }
  if (app.decoder != NULL) {
    (void)decoder_deinit(&app);
  }
  const h2_pal_result_t gizclaw_result = gizclaw_deinit(&app);
  const h2_pal_result_t audio_result = audio_deinit(&app);
  if (result == H2_PAL_OK) {
    result = gizclaw_result;
  }
  if (result == H2_PAL_OK) {
    result = audio_result;
  }
  ui_deinit(&app);
  return result;
}
