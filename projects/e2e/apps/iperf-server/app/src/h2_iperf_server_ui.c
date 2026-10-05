#include "h2_iperf_server_app_internal.h"
#include "h2_lvgl_platform.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

typedef struct ui_state {
  h2_iperf_server_app_t *app;
  lv_display_t *display;
  lv_indev_t *pointer;
  lv_obj_t *modes[3];
  lv_obj_t *action;
  lv_obj_t *action_label;
  lv_obj_t *status;
  lv_obj_t *addresses;
  lv_obj_t *rate[2];
  lv_obj_t *stream_label[2];
  uint16_t *pixels;
  h2_iperf_server_app_snapshot_t snapshot;
  uint64_t tick_ms;
  h2_pal_result_t error;
  lv_point_t point;
  bool pressed;
  uint32_t completed_flushes;
  unsigned touch_reads;
  bool touch_seen;
  unsigned touch_failures;
  bool touch_wait_up;
  uint32_t stride_bytes;
} ui_state_t;
static const h2_iperf_server_app_mode_t modes[] = {
    H2_IPERF_SERVER_APP_MODE_IPV4, H2_IPERF_SERVER_APP_MODE_IPV6,
    H2_IPERF_SERVER_APP_MODE_DUAL};
static void ui_error(ui_state_t *ui, const char *stage, int rc) {
  if (ui->error != H2_PAL_OK)
    return;
  ui->error = rc;
  char message[100];
  (void)snprintf(message, sizeof(message),
                 "H2_IPERF_SERVER_UI_ERROR stage=%s rc=%d", stage, rc);
  (void)h2_pal_log_write(ui->app->runtime->log, H2_PAL_LOG_ERROR,
                         "iperf-server", message);
}
static uint32_t tick(void) {
  lv_display_t *display = lv_display_get_default();
  ui_state_t *ui = display != NULL ? lv_display_get_user_data(display) : NULL;
  if (ui == NULL)
    return 0;
  int rc = h2_pal_time_get_monotonic_ms(ui->app->runtime->time, &ui->tick_ms);
  if (rc != H2_PAL_OK)
    ui_error(ui, "clock", rc);
  return (uint32_t)ui->tick_ms;
}
static void flush(lv_display_t *display, const lv_area_t *area,
                  uint8_t *pixels) {
  ui_state_t *ui = lv_display_get_user_data(display);
  const h2_display_rect_t rect = {.x = area->x1,
                                  .y = area->y1,
                                  .width = area->x2 - area->x1 + 1,
                                  .height = area->y2 - area->y1 + 1};
  int rc =
      h2_pal_display_draw_bitmap(ui->app->runtime->display, &rect, pixels,
                                 ui->stride_bytes, H2_DISPLAY_PIXEL_RGB565);
  const char *stage = "draw";
  if (rc == H2_PAL_OK) {
    stage = "present";
    rc = h2_pal_display_present(ui->app->runtime->display);
  }
  if (rc != H2_PAL_OK)
    ui_error(ui, stage, rc);
  else
    ++ui->completed_flushes;
  lv_display_flush_ready(display);
}
static void read_touch(lv_indev_t *pointer, lv_indev_data_t *data) {
  ui_state_t *ui = lv_indev_get_user_data(pointer);
  h2_pal_touch_event_t event;
  if (ui->touch_reads++ >= 16u) {
    data->point = ui->point;
    data->state =
        ui->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    return;
  }
  int rc = h2_pal_touch_poll_event(ui->app->runtime->touch, &event);
  if (rc == H2_PAL_ERR_IO || rc == H2_PAL_ERR_TIMEOUT) {
    if (ui->touch_failures++ == 0)
      (void)h2_pal_log_write(ui->app->runtime->log, H2_PAL_LOG_WARN,
                             "iperf-server", "H2_IPERF_SERVER_TOUCH_RETRY");
    ui->touch_wait_up = ui->touch_wait_up || ui->pressed;
    ui->pressed = false;
    /* Cancel the entire gesture: synthesizing only an UP would click the
     * button that happened to be held when the I2C read failed. */
    lv_indev_reset(pointer, NULL);
    data->point = ui->point;
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    if (ui->touch_failures >= 8u)
      ui_error(ui, "touch", rc);
    return;
  }
  if (ui->touch_failures != 0 &&
      (rc == H2_PAL_OK || rc == H2_PAL_ERR_WOULD_BLOCK)) {
    ui->touch_failures = 0;
    (void)h2_pal_log_write(ui->app->runtime->log, H2_PAL_LOG_INFO,
                           "iperf-server", "H2_IPERF_SERVER_TOUCH_RECOVERED");
  }
  if (rc == H2_PAL_OK || rc == H2_PAL_ERR_WOULD_BLOCK)
    ui->touch_seen = true;
  if (rc == H2_PAL_OK) {
    ui->point.x = event.x;
    ui->point.y = event.y;
    if (event.kind == H2_PAL_TOUCH_EVENT_UP)
      ui->touch_wait_up = false;
    ui->pressed = !ui->touch_wait_up && event.kind != H2_PAL_TOUCH_EVENT_UP;
    data->continue_reading = true;
  } else if (rc != H2_PAL_ERR_WOULD_BLOCK) {
    ui->pressed = false;
    ui_error(ui, "touch", rc);
  }
  data->point = ui->point;
  data->state = ui->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                       int width, uint32_t color) {
  lv_obj_t *object = lv_label_create(parent);
  if (object != NULL) {
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
  }
  return object;
}
static void select_mode(lv_event_t *event) {
  ui_state_t *ui = lv_event_get_user_data(event);
  lv_obj_t *object = lv_event_get_target_obj(event);
  for (unsigned i = 0; i < 3; ++i) {
    if (object == ui->modes[i]) {
      int rc = h2_iperf_server_app_request(ui->app, modes[i], false);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_BUSY)
        ui->error = rc;
    }
  }
}
static void toggle(lv_event_t *event) {
  ui_state_t *ui = lv_event_get_user_data(event);
  h2_iperf_server_app_snapshot_t snapshot;
  int rc = h2_iperf_server_app_snapshot(ui->app, &snapshot);
  if (rc == H2_PAL_OK) {
    bool start = snapshot.phase == H2_IPERF_SERVER_APP_STOPPED ||
                 snapshot.phase == H2_IPERF_SERVER_APP_ERROR;
    rc = h2_iperf_server_app_request(ui->app, snapshot.mode, start);
  }
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_BUSY)
    ui->error = rc;
}
static int build_ui(ui_state_t *ui, int width) {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_scrollable(screen, false);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b1220), 0);
  lv_obj_set_style_text_color(screen, lv_color_hex(0xf1f5f9), 0);
  lv_obj_t *title = label(screen, "iPerf", 18, 6, width - 36, 0xf1f5f9);
  if (title == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  lv_obj_set_style_text_font(title, &lv_font_montserrat_42, 0);
  if (label(screen, "WI-FI SPEED TEST SERVER", 20, 58, width - 40, 0x94a3b8) ==
      NULL)
    return H2_PAL_ERR_NO_MEMORY;
  const char *names[] = {"IPv4", "IPv6", "Dual stack"};
  int button_width = (width - 44) / 3;
  for (unsigned i = 0; i < 3; ++i) {
    ui->modes[i] = lv_button_create(screen);
    if (ui->modes[i] == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    lv_obj_set_pos(ui->modes[i], 18 + (int)i * (button_width + 4), 86);
    lv_obj_set_size(ui->modes[i], button_width, 42);
    lv_obj_set_style_radius(ui->modes[i], 12, 0);
    lv_obj_add_event_cb(ui->modes[i], select_mode, LV_EVENT_CLICKED, ui);
    lv_obj_t *text = lv_label_create(ui->modes[i]);
    if (text == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    lv_label_set_text(text, names[i]);
    lv_obj_center(text);
  }
  if (label(screen,
            "SSID  " H2_IPERF_SERVER_APP_SSID
            "\nPASS  " H2_IPERF_SERVER_APP_PASSWORD,
            20, 143, width - 40, 0xe2e8f0) == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  ui->addresses = label(screen, "", 20, 187, width - 40, 0x94a3b8);
  ui->status = label(screen, "", 20, 242, width - 40, 0x67e8f9);
  ui->action = lv_button_create(screen);
  if (ui->addresses == NULL || ui->status == NULL || ui->action == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  lv_obj_set_pos(ui->action, 18, 268);
  lv_obj_set_size(ui->action, width - 36, 48);
  lv_obj_set_style_radius(ui->action, 14, 0);
  lv_obj_add_event_cb(ui->action, toggle, LV_EVENT_CLICKED, ui);
  ui->action_label = lv_label_create(ui->action);
  if (ui->action_label == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  lv_obj_center(ui->action_label);
  for (unsigned i = 0; i < 2; ++i) {
    int x = 20 + (int)i * (width / 2);
    ui->stream_label[i] = label(screen, "", x, 334, width / 2 - 30, 0x94a3b8);
    ui->rate[i] = label(screen, "0.00", x, 355, width / 2 - 20, 0xf1f5f9);
    if (ui->rate[i] == NULL || ui->stream_label[i] == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    lv_obj_set_style_text_font(ui->rate[i], &lv_font_montserrat_42, 0);
  }
  return label(screen, "Mbit/s   |   TCP / UDP   |   iperf3", 20, 417,
               width - 40, 0x64748b) != NULL
             ? H2_PAL_OK
             : H2_PAL_ERR_NO_MEMORY;
}
static int refresh(ui_state_t *ui) {
  int rc = h2_iperf_server_app_snapshot(ui->app, &ui->snapshot);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_iperf_server_app_snapshot_t *state = &ui->snapshot;
  bool idle = state->phase == H2_IPERF_SERVER_APP_STOPPED ||
              state->phase == H2_IPERF_SERVER_APP_ERROR;
  for (unsigned i = 0; i < 3; ++i) {
    if (idle)
      lv_obj_remove_state(ui->modes[i], LV_STATE_DISABLED);
    else
      lv_obj_add_state(ui->modes[i], LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(
        ui->modes[i],
        lv_color_hex(state->mode == modes[i] ? 0x2563eb : 0x1e293b), 0);
  }
  char text[160];
  (void)snprintf(
      text, sizeof(text), "IPv4  %s\nIPv6  %s\nPort  %u",
      state->mode == H2_IPERF_SERVER_APP_MODE_IPV6
          ? "off"
          : (state->network.ipv4_text[0] ? state->network.ipv4_text
                                         : H2_IPERF_SERVER_APP_IPV4_TEXT),
      state->mode == H2_IPERF_SERVER_APP_MODE_IPV4
          ? "off"
          : (state->network.ipv6_text[0] ? state->network.ipv6_text
                                         : H2_IPERF_SERVER_APP_IPV6_TEXT),
      (unsigned)state->port);
  lv_label_set_text(ui->addresses, text);
  static const char *phases[] = {"Stopped", "Starting AP...", "Listening",
                                 "Stopping...", "Start failed"};
  h2_pal_wifi_ap_status_t ap = {0};
  (void)h2_pal_wifi_ap_get_status(ui->app->runtime->wifi_ap, &ap);
  if (state->error != H2_PAL_OK)
    (void)snprintf(text, sizeof(text), "%s  (%d)", phases[state->phase],
                   state->error);
  else if (state->phase == H2_IPERF_SERVER_APP_LISTENING) {
    int failed = 0;
    for (unsigned i = 0; i < 2; ++i)
      if (!state->streams[i].active && state->streams[i].result_code < 0)
        failed = state->streams[i].result_code;
    if (failed)
      (void)snprintf(text, sizeof(text), "Listening | last test failed (%d)",
                     failed);
    else
      (void)snprintf(text, sizeof(text), "Listening  |  %u client(s)",
                     (unsigned)ap.client_count);
  } else
    (void)snprintf(text, sizeof(text), "%s", phases[state->phase]);
  lv_label_set_text(ui->status, text);
  lv_label_set_text(ui->action_label,
                    idle ? "Start server"
                    : state->phase == H2_IPERF_SERVER_APP_STOPPING
                        ? "Stopping..."
                        : "Stop server");
  lv_obj_set_style_bg_color(ui->action,
                            lv_color_hex(idle ? 0x0891b2 : 0xbe123c), 0);
  if (state->phase == H2_IPERF_SERVER_APP_STOPPING)
    lv_obj_add_state(ui->action, LV_STATE_DISABLED);
  else
    lv_obj_remove_state(ui->action, LV_STATE_DISABLED);
  for (unsigned i = 0; i < 2; ++i) {
    const h2_iperf_server_app_stream_t *stream = &state->streams[i];
    uint64_t bps =
        stream->progress.duration_ms == 0
            ? 0
            : stream->progress.bytes * 8000u / stream->progress.duration_ms;
    (void)snprintf(text, sizeof(text), "%llu.%02llu",
                   (unsigned long long)(bps / 1000000u),
                   (unsigned long long)((bps / 10000u) % 100u));
    lv_label_set_text(ui->rate[i], text);
    (void)snprintf(text, sizeof(text), "IPv%u %s %s", i == 0 ? 4u : 6u,
                   stream->progress.protocol == H2_IPERF_PROTOCOL_UDP ? "UDP"
                                                                      : "TCP",
                   stream->progress.sending ? "TX" : "RX");
    lv_label_set_text(ui->stream_label[i], text);
  }
  return H2_PAL_OK;
}
h2_pal_result_t h2_iperf_server_app_run_ui(h2_iperf_server_app_t *app,
                                           bool (*should_exit)(void *user),
                                           void *user) {
  if (app == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  ui_state_t ui = {.app = app};
  h2_runtime_t *runtime = app->runtime;
  bool display_open = false, touch_open = false, platform_open = false,
       lvgl_open = false;
  int rc = h2_pal_display_open(runtime->display);
  if (rc != H2_PAL_OK)
    return rc;
  display_open = true;
  h2_display_info_t info = {0};
  rc = h2_pal_display_get_info(runtime->display, &info);
  if (rc != H2_PAL_OK)
    goto done;
  if (info.width < 340 || info.width > 1024 || info.height < 440 ||
      info.height > 4096) {
    rc = H2_PAL_ERR_UNSUPPORTED;
    goto done;
  }
  rc = h2_pal_touch_open(runtime->touch);
  if (rc != H2_PAL_OK)
    goto done;
  touch_open = true;
  h2_pal_touch_info_t touch_info = {0};
  rc = h2_pal_touch_get_info(runtime->touch, &touch_info);
  if (rc != H2_PAL_OK)
    goto done;
  if (touch_info.width != (uint32_t)info.width ||
      touch_info.height != (uint32_t)info.height) {
    rc = H2_PAL_ERR_INVALID_ARG;
    goto done;
  }
  const h2_lvgl_platform_config_t platform = {.allocator = runtime->mem,
                                              .task_api = runtime->task,
                                              .sync_api = runtime->sync,
                                              .queue_api = runtime->queue,
                                              .time_api = runtime->time};
  rc = h2_lvgl_platform_init(&platform);
  if (rc != H2_PAL_OK)
    goto done;
  platform_open = true;
  lv_init();
  lvgl_open = true;
  ui.stride_bytes =
      lv_draw_buf_width_to_stride((uint32_t)info.width, LV_COLOR_FORMAT_RGB565);
  size_t buffer_bytes = (size_t)ui.stride_bytes * (size_t)info.height;
  ui.pixels = h2_pal_mem_alloc(runtime->mem, buffer_bytes);
  if (ui.pixels == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  ui.display = lv_display_create(info.width, info.height);
  if (ui.display == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  lv_display_set_default(ui.display);
  lv_display_set_user_data(ui.display, &ui);
  lv_tick_set_cb(tick);
  lv_display_set_color_format(ui.display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(ui.display, flush);
  lv_display_set_buffers(ui.display, ui.pixels, NULL, buffer_bytes,
                         LV_DISPLAY_RENDER_MODE_FULL);
  ui.pointer = lv_indev_create();
  if (ui.pointer == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  lv_indev_set_type(ui.pointer, LV_INDEV_TYPE_POINTER);
  lv_indev_set_user_data(ui.pointer, &ui);
  lv_indev_set_read_cb(ui.pointer, read_touch);
  rc = build_ui(&ui, info.width);
  if (rc != H2_PAL_OK)
    goto done;
  rc = h2_pal_display_set_brightness_percent(runtime->display, 80);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_UNSUPPORTED)
    goto done;
  rc = H2_PAL_OK;
  uint64_t last_refresh = 0;
  bool ready = false;
  while (ui.error == H2_PAL_OK && (should_exit == NULL || !should_exit(user))) {
    uint64_t now;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK)
      break;
    if (now - last_refresh >= 200u || last_refresh == 0) {
      rc = refresh(&ui);
      if (rc != H2_PAL_OK)
        break;
      last_refresh = now;
    }
    ui.touch_reads = 0;
    (void)lv_timer_handler();
    if (!ready && ui.completed_flushes != 0 && ui.touch_seen &&
        ui.error == H2_PAL_OK) {
      ready = true;
      rc = app->config.ui_ready != NULL
               ? app->config.ui_ready(app->config.ui_user)
               : H2_PAL_OK;
      if (rc != H2_PAL_OK)
        break;
      (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "iperf-server",
                             "H2_IPERF_SERVER_UI_READY touch=1 display=1");
    }
    rc = h2_pal_time_sleep_ms(runtime->time, 10u);
    if (rc != H2_PAL_OK)
      break;
  }
  if (ui.error != H2_PAL_OK)
    rc = ui.error;
done:
  if (ui.pointer != NULL)
    lv_indev_delete(ui.pointer);
  if (ui.display != NULL)
    lv_display_delete(ui.display);
  if (lvgl_open)
    lv_deinit();
  h2_pal_mem_free(runtime->mem, ui.pixels);
  if (platform_open)
    h2_lvgl_platform_deinit();
  if (touch_open) {
    int closed = h2_pal_touch_close(runtime->touch);
    if (rc == H2_PAL_OK)
      rc = closed;
  }
  if (display_open) {
    int closed = h2_pal_display_close(runtime->display);
    if (rc == H2_PAL_OK)
      rc = closed;
  }
  return rc;
}
