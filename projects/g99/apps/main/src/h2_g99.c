#include "os/h2_pal_log.h"
#include "h2_g99_internal.h"
#include "h2_runtime_input_button.h"
#include "h2_runtime_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define H2_G99_LOOP_MS 5u
#define H2_G99_BATTERY_PERIOD_MS 5000u
#define H2_G99_RESCAN_MS 15000u

void h2_g99_log(const h2_g99_app_t* app, const char* format, ...) {
  if (app == NULL || app->runtime == NULL || format == NULL) return;
  char message[256];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  (void)h2_pal_log_write(app->runtime->log, H2_PAL_LOG_INFO, "g99", message);
}

static uint64_t now_ms(const h2_g99_app_t* app) {
  uint64_t value = 0u;
  (void)h2_pal_time_get_monotonic_ms(app->runtime->time, &value);
  return value;
}

uint64_t h2_g99_now_us(const h2_g99_app_t* app) {
  uint64_t value = 0u;
  if (h2_pal_time_get_monotonic_us(app->runtime->time, &value) == H2_PAL_OK)
    return value;
  value = 0u;
  (void)h2_pal_time_get_monotonic_ms(app->runtime->time, &value);
  return value * 1000u;
}

static uint32_t clamp_u32(uint64_t value) {
  return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* Hands the statistics of a presented frame to config->frame. work_us is
 * the loop work of the iteration that presented it. */
static void report_frame(h2_g99_app_t* app, uint64_t work_us) {
  if (!app->frame_presented) return;
  app->frame_presented = false;
  const uint64_t now = h2_g99_now_us(app);
  ++app->frame_index;
  const h2_g99_frame_stats_t stats = {
      .index = app->frame_index,
      .interval_us =
          app->frame_index > 1u ? clamp_u32(now - app->frame_last_us) : 0u,
      .work_us = clamp_u32(work_us),
      .draw_us = clamp_u32(app->frame_draw_us),
      .present_us = clamp_u32(app->frame_present_us),
      .pixels = clamp_u32(app->frame_pixels),
  };
  app->frame_last_us = now;
  app->frame_draw_us = 0u;
  app->frame_present_us = 0u;
  app->frame_pixels = 0u;
  if (app->config->frame != NULL)
    app->config->frame(app->config->frame_user, &stats);
}

/* ---------- settings effects ---------- */

static void apply_settings(h2_g99_app_t* app) {
  if (app->nav.screen_on)
    (void)h2_pal_display_set_brightness_percent(
        app->runtime->display, app->settings.brightness_percent);
  if (app->runtime->audio != NULL)
    (void)h2_pal_audio_set_speaker_volume_percent(app->runtime->audio,
                                                  app->settings.volume_percent);
  h2_g99_nav_set_screen_timeout(
      &app->nav, h2_g99_screen_timeout_ms(app->settings.screen_timeout_index),
      app->now_ms);
}

static void save_settings(h2_g99_app_t* app) {
  const h2_pal_result_t rc = h2_g99_settings_save(
      app->runtime->pref, app->config->settings_namespace, &app->settings);
  if (rc != H2_PAL_OK) h2_g99_log(app, "settings save rc=%d", (int)rc);
}

/* ---------- Wi-Fi ---------- */

static void maybe_scan(h2_g99_app_t* app) {
  if (app->wifi_state.phase != H2_G99_WIFI_IDLE) return;
  if (app->scanned && app->now_ms - app->last_scan_ms < H2_G99_RESCAN_MS)
    return;
  if (h2_g99_wifi_request_scan(app->wifi) == H2_PAL_OK) {
    app->scanned = true;
    app->last_scan_ms = app->now_ms;
  }
}

static bool wifi_page_visible(const h2_g99_app_t* app) {
  return app->nav.route == H2_G99_ROUTE_SETTINGS &&
         app->nav.anim == H2_G99_TRANSITION_NONE &&
         app->nav.sub_sel == H2_G99_SETTING_WIFI;
}

static void poll_wifi(h2_g99_app_t* app) {
  h2_g99_wifi_snapshot_t next;
  h2_g99_wifi_get(app->wifi, &next);
  if (next.version == app->wifi_state.version &&
      next.phase == app->wifi_state.phase)
    return;
  const bool finished_connect =
      next.connects_done != app->wifi_state.connects_done;
  app->wifi_state = next;
  app->info_rev++;
  if (app->nav.editing && app->nav.sub_sel == H2_G99_SETTING_WIFI) {
    if (finished_connect && next.last_result == H2_PAL_OK) {
      h2_g99_nav_end_edit(&app->nav);
    } else {
      h2_g99_nav_set_edit_count(&app->nav, next.network_count,
                                app->nav.edit_index);
    }
  }
  if (finished_connect)
    h2_g99_log(app, "wifi connect rc=%d connected=%d", (int)next.last_result,
               next.connected ? 1 : 0);
}

static void activate_wifi(h2_g99_app_t* app, uint8_t index) {
  if (index >= app->wifi_state.network_count) return;
  const h2_g99_wifi_network_t* network = &app->wifi_state.networks[index];
  if (network->security == H2_PAL_WIFI_SECURITY_OPEN) {
    (void)h2_g99_wifi_request_connect(app->wifi, network->ssid, "");
    return;
  }
  h2_g99_ui_open_keyboard(app, network->ssid);
  h2_g99_nav_set_text_entry(&app->nav, app->keyboard_panel != NULL);
}

static void submit_password(h2_g99_app_t* app) {
  char password[H2_PAL_WIFI_PASSWORD_MAX + 1];
  char ssid[sizeof(app->keyboard_ssid)];
  memcpy(ssid, app->keyboard_ssid, sizeof(ssid));
  h2_g99_ui_close_keyboard(app, password, sizeof(password));
  h2_g99_nav_set_text_entry(&app->nav, false);
  const h2_pal_result_t rc =
      h2_g99_wifi_request_connect(app->wifi, ssid, password);
  memset(password, 0, sizeof(password));
  if (rc != H2_PAL_OK) h2_g99_log(app, "wifi connect request rc=%d", (int)rc);
  app->info_rev++;
}

/* ---------- navigation results ---------- */

static void begin_edit(h2_g99_app_t* app) {
  switch (app->nav.sub_sel) {
    case H2_G99_SETTING_WIFI:
      h2_g99_nav_set_edit_count(&app->nav, app->wifi_state.network_count, 0u);
      app->scanned = false;
      maybe_scan(app);
      break;
    case H2_G99_SETTING_SCREEN_TIMEOUT:
      h2_g99_nav_set_edit_count(&app->nav, H2_G99_SCREEN_TIMEOUT_COUNT,
                                app->settings.screen_timeout_index);
      break;
    default:
      h2_g99_nav_set_edit_count(&app->nav, 0u, 0u);
      break;
  }
}

static void step_value(h2_g99_app_t* app, int direction) {
  if (app->nav.sub_sel == H2_G99_SETTING_VOLUME)
    (void)h2_g99_settings_step_volume(&app->settings, direction);
  else if (app->nav.sub_sel == H2_G99_SETTING_BRIGHTNESS)
    (void)h2_g99_settings_step_brightness(&app->settings, direction);
  else
    return;
  apply_settings(app);
  save_settings(app);
}

static void activate_edit(h2_g99_app_t* app) {
  switch (app->nav.sub_sel) {
    case H2_G99_SETTING_WIFI:
      activate_wifi(app, app->nav.edit_index);
      break;
    case H2_G99_SETTING_SCREEN_TIMEOUT:
      app->settings.screen_timeout_index = app->nav.edit_index;
      apply_settings(app);
      save_settings(app);
      h2_g99_nav_end_edit(&app->nav);
      break;
    default:
      h2_g99_nav_end_edit(&app->nav);
      break;
  }
}

static h2_pal_result_t handle(h2_g99_app_t* app, h2_g99_action_t action) {
  const h2_g99_nav_result_t result =
      h2_g99_nav_input(&app->nav, action, app->now_ms);
  h2_pal_result_t rc = H2_PAL_OK;
  switch (result) {
    case H2_G99_NAV_IGNORED:
    case H2_G99_NAV_FORWARD_TO_APP:
      return H2_PAL_OK;
    case H2_G99_NAV_LAUNCH_APP:
      if (h2_g99_lua_launch(app) != H2_PAL_OK) {
        if (app->host != NULL) return H2_PAL_ERR_INVALID_STATE;
        app->nav.route = H2_G99_ROUTE_LIBRARY;
      }
      break;
    case H2_G99_NAV_EXIT_APP:
      rc = h2_g99_lua_stop(app);
      break;
    case H2_G99_NAV_SCREEN_OFF:
      h2_g99_ui_set_screen(app, false);
      break;
    case H2_G99_NAV_SCREEN_ON:
      h2_g99_ui_set_screen(app, true);
      break;
    case H2_G99_NAV_EDIT_BEGIN:
      begin_edit(app);
      break;
    case H2_G99_NAV_EDIT_INCREASE:
      step_value(app, 1);
      break;
    case H2_G99_NAV_EDIT_DECREASE:
      step_value(app, -1);
      break;
    case H2_G99_NAV_EDIT_ACTIVATE:
      activate_edit(app);
      break;
    case H2_G99_NAV_TEXT_SUBMIT:
      submit_password(app);
      break;
    case H2_G99_NAV_TEXT_CANCEL:
      h2_g99_ui_close_keyboard(app, NULL, 0u);
      break;
    default:
      break;
  }
  /* The info area follows a wheel rotation by its selection alone; a
   * rebuild would redraw it on top of every rotation. */
  if (result != H2_G99_NAV_CHANGED || app->nav.anim != H2_G99_TRANSITION_ROTATE)
    app->info_rev++;
  return rc;
}

static void handle_touch(h2_g99_app_t* app) {
  h2_g99_touch_request_t request = app->touch_request;
  app->touch_request = (h2_g99_touch_request_t){-1, -1, -1, H2_G99_TEXT_NONE};
  if (request.text == H2_G99_TEXT_SUBMIT && app->nav.text_entry)
    (void)handle(app, H2_G99_ACTION_OK);
  if (request.text == H2_G99_TEXT_CANCEL && app->nav.text_entry)
    (void)handle(app, H2_G99_ACTION_HOME);
  if (request.action >= 0) (void)handle(app, (h2_g99_action_t)request.action);
  if (request.row >= 0 && app->nav.route == H2_G99_ROUTE_SETTINGS &&
      h2_g99_nav_input_ready(&app->nav)) {
    if (!app->nav.editing) (void)handle(app, H2_G99_ACTION_OK);
    if (app->nav.editing && app->nav.edit_count > 0u) {
      h2_g99_nav_set_edit_count(&app->nav, app->nav.edit_count,
                                (uint8_t)request.row);
      (void)handle(app, H2_G99_ACTION_OK);
    }
  }
  if (request.value >= 0 && app->nav.route == H2_G99_ROUTE_SETTINGS &&
      h2_g99_nav_input_ready(&app->nav)) {
    if (app->nav.sub_sel == H2_G99_SETTING_VOLUME)
      app->settings.volume_percent = (uint8_t)request.value;
    else if (app->nav.sub_sel == H2_G99_SETTING_BRIGHTNESS)
      app->settings.brightness_percent = (uint8_t)request.value;
    else
      return;
    h2_g99_settings_normalize(&app->settings);
    apply_settings(app);
    save_settings(app);
    app->info_rev++;
  }
}

/* ---------- Runtime events ---------- */

static bool action_of(h2_runtime_component_id_t id, h2_g99_action_t* out) {
  switch (id) {
    case H2_G99_COMPONENT_HOME:
      *out = H2_G99_ACTION_HOME;
      return true;
    case H2_G99_COMPONENT_SCREEN:
      *out = H2_G99_ACTION_SCREEN;
      return true;
    case H2_G99_COMPONENT_UP:
      *out = H2_G99_ACTION_UP;
      return true;
    case H2_G99_COMPONENT_DOWN:
      *out = H2_G99_ACTION_DOWN;
      return true;
    case H2_G99_COMPONENT_OK:
      *out = H2_G99_ACTION_OK;
      return true;
    default:
      return false;
  }
}

/* Logical text input replaces the password while its keyboard is open. */
static void handle_custom(h2_g99_app_t* app,
                          const h2_runtime_custom_event_payload_t* custom,
                          size_t payload_size) {
  if (payload_size < H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE ||
      custom->id != H2_G99_EVENT_TEXT_INPUT || !app->nav.text_entry ||
      custom->size > H2_PAL_WIFI_PASSWORD_MAX)
    return;
  char text[H2_PAL_WIFI_PASSWORD_MAX + 1];
  memcpy(text, custom->data, custom->size);
  text[custom->size] = '\0';
  h2_g99_ui_set_keyboard_text(app, text);
  memset(text, 0, sizeof(text));
  app->nav.last_input_ms = app->now_ms;
}

static h2_pal_result_t poll_events(h2_g99_app_t* app) {
  h2_runtime_event_payload_buffer_t payload;
  for (;;) {
    h2_runtime_event_t event = {.payload = &payload,
                                .payload_capacity = sizeof(payload)};
    const h2_pal_result_t rc = h2_runtime_poll_event(app->runtime, &event);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      return H2_PAL_OK;
    if (rc != H2_PAL_OK) return rc;
    if (event.kind == H2_RUNTIME_EVENT_CUSTOM) {
      handle_custom(app, &payload.custom, event.payload_size);
      continue;
    }
    h2_g99_action_t action;
    if (event.kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN ||
        event.payload_size < sizeof(h2_runtime_button_down_event_t) ||
        !action_of(event.component_id, &action))
      continue;
    /* BUTTON_DOWN repeats while held; act once per physical press. A press
     * that arrives while input is gated is consumed, never replayed. */
    const h2_runtime_button_down_event_t* down = event.payload;
    h2_runtime_timestamp_ms_t* last = &app->last_press[event.component_id];
    if (*last == down->pressed_at_ms) continue;
    *last = down->pressed_at_ms;
    const h2_pal_result_t handled = handle(app, action);
    if (handled != H2_PAL_OK) return handled;
  }
}

/* The status bar shows HH:MM; rebuild the page when the minute changes. */
static void poll_clock(h2_g99_app_t* app) {
  uint64_t wall_ms = 0u;
  if (h2_pal_time_get_wall_ms(app->runtime->time, &wall_ms) != H2_PAL_OK)
    return;
  const uint64_t minute = wall_ms / 60000u;
  if (minute == app->shown_minute) return;
  app->shown_minute = minute;
  app->info_rev++;
}

static void poll_battery(h2_g99_app_t* app) {
  if (app->now_ms - app->last_battery_ms < H2_G99_BATTERY_PERIOD_MS &&
      app->last_battery_ms != 0u)
    return;
  app->last_battery_ms = app->now_ms;
  h2_runtime_battery_state_t state;
  const bool valid =
      h2_runtime_component_state_battery(app->runtime, H2_G99_COMPONENT_BATTERY,
                                         &state) == H2_PAL_OK &&
      state.result == H2_PAL_OK;
  const uint8_t percent =
      valid ? (uint8_t)(state.reading.percent_x100 / 100u) : 0u;
  if (valid != app->battery_valid || percent != app->battery_percent) {
    app->battery_valid = valid;
    app->battery_percent = percent > 100u ? 100u : percent;
    app->info_rev++;
  }
}

/* ---------- observation ---------- */

static h2_g99_wifi_status_t wifi_status(h2_g99_wifi_phase_t phase) {
  switch (phase) {
    case H2_G99_WIFI_IDLE:
      return H2_G99_WIFI_STATUS_IDLE;
    case H2_G99_WIFI_SCANNING:
      return H2_G99_WIFI_STATUS_SCANNING;
    case H2_G99_WIFI_CONNECTING:
      return H2_G99_WIFI_STATUS_CONNECTING;
    default:
      return H2_G99_WIFI_STATUS_UNAVAILABLE;
  }
}

/* Geometry of the page Main shows at rest; nothing while an app runs. */
static void observe_layout(const h2_g99_app_t* app,
                           h2_g99_observation_t* next) {
  next->wheel_rect = (h2_g99_rect_t){0};
  next->info_rect = (h2_g99_rect_t){0};
  if (app->nav.route == H2_G99_ROUTE_APP) return;
  const h2_g99_route_t route =
      app->nav.route == H2_G99_ROUTE_BOOT ? H2_G99_ROUTE_MAIN : app->nav.route;
  h2_g99_wheel_bounds(
      &app->layout,
      route == H2_G99_ROUTE_LIBRARY ? H2_G99_WHEEL_THUMBS : H2_G99_WHEEL_ICONS,
      h2_g99_nav_route_count(&app->nav, route), &next->wheel_rect);
  const h2_g99_layout_t* l = &app->layout;
  next->info_rect = (h2_g99_rect_t){
      l->panel_x, l->panel_y, l->panel_x + l->panel_w, l->panel_y + l->panel_h};
}

static void observe_wifi(const h2_g99_app_t* app, h2_g99_observation_t* next) {
  const h2_g99_wifi_snapshot_t* w = &app->wifi_state;
  next->wifi_status = wifi_status(w->phase);
  next->wifi_connected = w->connected;
  memset(next->wifi_ssid, 0, sizeof(next->wifi_ssid));
  if (w->connected)
    (void)snprintf(next->wifi_ssid, sizeof(next->wifi_ssid), "%s",
                   w->connected_ssid);
  next->wifi_network_count = w->network_count;
  memset(next->wifi_focused_ssid, 0, sizeof(next->wifi_focused_ssid));
  if (app->nav.editing && app->nav.route == H2_G99_ROUTE_SETTINGS &&
      app->nav.sub_sel == H2_G99_SETTING_WIFI &&
      app->nav.edit_index < w->network_count)
    (void)snprintf(next->wifi_focused_ssid, sizeof(next->wifi_focused_ssid),
                   "%s", w->networks[app->nav.edit_index].ssid);
  next->wifi_last_result = (int32_t)w->last_result;
}

static void publish(h2_g99_app_t* app) {
  h2_g99_observation_t next = app->observation;
  next.lifecycle = app->lifecycle;
  h2_g99_nav_observe(&app->nav, &next);
  next.volume_percent = app->settings.volume_percent;
  next.brightness_percent = app->settings.brightness_percent;
  next.screen_timeout_index = app->settings.screen_timeout_index;
  next.width = app->layout.width;
  next.height = app->layout.height;
  next.settings_loaded = app->settings_loaded;
  next.text_entry = app->nav.text_entry;
  observe_wifi(app, &next);
  observe_layout(app, &next);
  next.wheel_drag = app->dragging ? app->drag_offset : 0.0f;
  next.sequence = app->observation.sequence;
  if (app->observation_sent &&
      memcmp(&next, &app->observation, sizeof(next)) == 0)
    return;
  next.sequence++;
  app->observation = next;
  app->observation_sent = true;
  if (app->config->observe != NULL)
    app->config->observe(app->config->observe_user, &app->observation);
}

/* ---------- run ---------- */

static void read_device_id(h2_g99_app_t* app) {
  uint8_t mac[6];
  if (app->runtime->wifi_sta == NULL ||
      h2_pal_wifi_sta_get_mac(app->runtime->wifi_sta, mac) != H2_PAL_OK)
    return;
  (void)snprintf(app->device_id, sizeof(app->device_id),
                 "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
}

static h2_pal_result_t open_display(h2_g99_app_t* app,
                                    h2_display_info_t* info) {
  int rc = h2_pal_display_get_info(app->runtime->display, info);
  if (rc != H2_PAL_OK) {
    rc = h2_pal_display_open(app->runtime->display);
    if (rc != H2_PAL_OK) return (h2_pal_result_t)rc;
    app->display_opened = true;
    rc = h2_pal_display_get_info(app->runtime->display, info);
  }
  return (h2_pal_result_t)rc;
}

static h2_pal_result_t loop(h2_g99_app_t* app) {
  const h2_g99_config_t* config = app->config;
  while (config->should_stop == NULL ||
         config->should_stop(config->should_stop_user) == 0) {
    app->now_ms = now_ms(app);
    h2_pal_result_t rc = poll_events(app);
    if (rc != H2_PAL_OK) return rc;
    handle_touch(app);
    const bool rotating = app->nav.anim == H2_G99_TRANSITION_ROTATE;
    const h2_g99_nav_result_t tick = h2_g99_nav_tick(&app->nav, app->now_ms);
    if (tick == H2_G99_NAV_SCREEN_OFF) h2_g99_ui_set_screen(app, false);
    if (tick != H2_G99_NAV_IGNORED && !(rotating && tick == H2_G99_NAV_CHANGED))
      app->info_rev++;
    if (app->lifecycle == H2_G99_LIFECYCLE_STARTING &&
        app->nav.route != H2_G99_ROUTE_BOOT)
      app->lifecycle = H2_G99_LIFECYCLE_RUNNING;
    poll_wifi(app);
    poll_battery(app);
    poll_clock(app);
    if (wifi_page_visible(app)) maybe_scan(app);
    if (app->nav.route == H2_G99_ROUTE_APP) {
      if (!h2_g99_lua_poll(app)) {
        app->nav.route = H2_G99_ROUTE_LIBRARY;
        rc = h2_g99_lua_stop(app);
        if (rc != H2_PAL_OK) return rc;
      }
    } else {
      const uint64_t work_start = h2_g99_now_us(app);
      h2_g99_ui_frame(app);
      h2_g99_ui_pump(app);
      if (app->flush_result != H2_PAL_OK) {
        h2_g99_log(app, "display flush rc=%d", (int)app->flush_result);
        return app->flush_result;
      }
      report_frame(app, h2_g99_now_us(app) - work_start);
    }
    publish(app);
    (void)h2_pal_time_sleep_ms(app->runtime->time, H2_G99_LOOP_MS);
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_g99_run(h2_runtime_t* runtime,
                           const h2_g99_config_t* config) {
  if (runtime == NULL || config == NULL || runtime->display == NULL ||
      runtime->time == NULL || runtime->mem == NULL || runtime->task == NULL ||
      runtime->sync == NULL || runtime->queue == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_g99_app_t* app = h2_pal_mem_alloc(runtime->mem, sizeof(*app));
  if (app == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(app, 0, sizeof(*app));
  app->runtime = runtime;
  app->config = config;
  app->lifecycle = H2_G99_LIFECYCLE_STARTING;
  app->now_ms = now_ms(app);
  h2_display_info_t info = {0};
  h2_pal_result_t rc = open_display(app, &info);
  if (rc == H2_PAL_OK &&
      !h2_g99_layout_compute(info.width, info.height, &app->layout))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    const h2_pal_result_t load_rc = h2_g99_settings_load(
        runtime->pref, config->settings_namespace, &app->settings);
    app->settings_loaded = load_rc == H2_PAL_OK;
    if (load_rc != H2_PAL_OK)
      h2_g99_log(app, "settings load rc=%d", (int)load_rc);
    read_device_id(app);
    rc = h2_g99_wifi_create(runtime, &app->wifi);
    if (rc != H2_PAL_OK) {
      h2_g99_log(app, "wifi worker rc=%d", (int)rc);
      app->wifi = NULL;
      rc = H2_PAL_OK;
    }
    h2_g99_wifi_get(app->wifi, &app->wifi_state);
  }
  if (rc == H2_PAL_OK) rc = h2_g99_ui_init(app);
  if (rc == H2_PAL_OK) {
    /* Rasterizing the artwork can take a noticeable time on slow targets;
     * start the 900 ms boot animation only once its first frame can draw. */
    app->now_ms = now_ms(app);
    app->last_tick_ms = app->now_ms;
    h2_g99_nav_init(&app->nav, (uint8_t)H2_G99_LIBRARY_COUNT, app->now_ms);
    apply_settings(app);
    h2_g99_log(app, "main ready %dx%d k=%.2f", (int)info.width,
               (int)info.height, (double)app->layout.k);
    publish(app);
    rc = loop(app);
  }
  app->lifecycle =
      rc == H2_PAL_OK ? H2_G99_LIFECYCLE_STOPPING : H2_G99_LIFECYCLE_FAILED;
  publish(app);
  if (app->host != NULL) {
    const h2_pal_result_t stop_rc = h2_g99_lua_stop(app);
    if (rc == H2_PAL_OK) rc = stop_rc;
  }
  h2_g99_ui_close_keyboard(app, NULL, 0u);
  h2_g99_ui_deinit(app);
  const h2_pal_result_t wifi_rc = h2_g99_wifi_destroy(app->wifi);
  if (rc == H2_PAL_OK) rc = wifi_rc;
  if (app->display_opened) (void)h2_pal_display_close(runtime->display);
  if (rc == H2_PAL_OK) app->lifecycle = H2_G99_LIFECYCLE_STOPPED;
  publish(app);
  h2_pal_mem_free(runtime->mem, app);
  return rc;
}
