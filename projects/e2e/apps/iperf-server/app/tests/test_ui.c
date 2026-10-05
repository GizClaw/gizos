#include "h2_desktop_platform.h"
#include "h2_iperf_server_app.h"
#include "h2_iperf_test_support.h"
#include "lvgl.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t framebuffer[368 * 448];
typedef struct fixture {
  h2_iperf_server_app_t *app;
  const h2_pal_time_api_t *time;
  bool ready;
  bool pressed;
  bool bad_viewport;
  bool fail_draw;
  bool fail_touch;
  unsigned touch_errors;
  bool retry_cancelled_click;
  bool retried_stop;
  unsigned stop_attempts;
  unsigned step;
  unsigned display_open, display_close, touch_open, touch_close, starts, stops;
  uint64_t deadline, next_edge;
} fixture_t;
static uint64_t now(fixture_t *f) {
  uint64_t ms;
  assert(h2_pal_time_get_monotonic_ms(f->time, &ms) == H2_PAL_OK);
  return ms;
}
static int network_start(void *user, h2_iperf_server_app_mode_t mode,
                         h2_iperf_server_app_network_t *out) {
  fixture_t *f = user;
  ++f->starts;
  if (mode != H2_IPERF_SERVER_APP_MODE_IPV6) {
    out->ipv4 = h2_iperf_test_loopback(0);
    strcpy(out->ipv4_text, "127.0.0.1");
  }
  if (mode != H2_IPERF_SERVER_APP_MODE_IPV4) {
    out->ipv6.family = H2_PAL_NET_FAMILY_IPV6;
    out->ipv6.ip[15] = 1;
    strcpy(out->ipv6_text, "::1");
  }
  return H2_PAL_OK;
}
static int network_stop(void *user) {
  fixture_t *f = user;
  if (++f->stop_attempts == 1u)
    return H2_PAL_ERR_IO;
  ++f->stops;
  return H2_PAL_OK;
}
static int display_open(void *user) {
  ++((fixture_t *)user)->display_open;
  return H2_PAL_OK;
}
static int display_info(void *user, h2_display_info_t *info) {
  (void)user;
  *info = (h2_display_info_t){
      .width = 368, .height = 448, .native_format = H2_DISPLAY_PIXEL_RGB565};
  return H2_PAL_OK;
}
static int draw(void *user, const h2_display_rect_t *rect, const void *pixels,
                size_t stride, h2_display_pixel_format_t format) {
  if (((fixture_t *)user)->fail_draw)
    return H2_PAL_ERR_IO;
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  assert(rect->x >= 0 && rect->y >= 0 && rect->x + rect->width <= 368 &&
         rect->y + rect->height <= 448);
  for (int y = 0; y < rect->height; ++y)
    memcpy(framebuffer + (rect->y + y) * 368 + rect->x,
           (const uint8_t *)pixels + (size_t)y * stride,
           (size_t)rect->width * 2);
  return H2_PAL_OK;
}
static int present(void *user) {
  (void)user;
  return H2_PAL_OK;
}
static int display_close(void *user) {
  ++((fixture_t *)user)->display_close;
  return H2_PAL_OK;
}
static int touch_open(void *user) {
  ++((fixture_t *)user)->touch_open;
  return H2_PAL_OK;
}
static int touch_info(void *user, h2_pal_touch_info_t *info) {
  *info = (h2_pal_touch_info_t){
      .width = 368, .height = ((fixture_t *)user)->bad_viewport ? 449 : 448};
  return H2_PAL_OK;
}
static const int taps[9][2] = {{70, 105},  {180, 290}, {180, 290},
                               {180, 105}, {180, 290}, {180, 290},
                               {290, 105}, {180, 290}, {180, 290}};
static bool tap_ready(unsigned step) {
  lv_obj_t *screen = lv_screen_active();
  for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
    lv_obj_t *object = lv_obj_get_child(screen, (int32_t)i);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    if (taps[step][0] >= area.x1 && taps[step][0] <= area.x2 &&
        taps[step][1] >= area.y1 && taps[step][1] <= area.y2 &&
        lv_obj_is_clickable(object) &&
        !lv_obj_has_state(object, LV_STATE_DISABLED))
      return true;
  }
  return false;
}
static bool retry_stop_ready(void) {
  lv_obj_t *screen = lv_screen_active();
  for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
    lv_obj_t *object = lv_obj_get_child(screen, (int32_t)i);
    if (!lv_obj_is_clickable(object) ||
        lv_obj_has_state(object, LV_STATE_DISABLED))
      continue;
    for (uint32_t j = 0; j < lv_obj_get_child_count(object); ++j) {
      lv_obj_t *child = lv_obj_get_child(object, (int32_t)j);
      if (lv_obj_check_type(child, &lv_label_class) &&
          strcmp(lv_label_get_text(child), "Retry stop") == 0)
        return true;
    }
  }
  return false;
}
static int touch_poll(void *user, h2_pal_touch_event_t *event) {
  fixture_t *f = user;
  if (f->fail_touch)
    return H2_PAL_ERR_IO;
  if (f->ready && f->step == 0 && f->touch_errors == 0) {
    ++f->touch_errors;
    return H2_PAL_ERR_TIMEOUT;
  }
  if (f->ready && f->step == 1 && f->pressed && f->touch_errors == 1) {
    ++f->touch_errors;
    f->retry_cancelled_click = true;
    return H2_PAL_ERR_IO;
  }
  if (!f->ready || f->step == 9 || now(f) < f->next_edge)
    return H2_PAL_ERR_WOULD_BLOCK;
  h2_iperf_server_app_snapshot_t snapshot;
  assert(h2_iperf_server_app_snapshot(f->app, &snapshot) == H2_PAL_OK);
  if (f->step == 3u && !f->retried_stop &&
      snapshot.phase == H2_IPERF_SERVER_APP_STOPPING &&
      snapshot.error == H2_PAL_ERR_IO) {
    if (!retry_stop_ready())
      return H2_PAL_ERR_WOULD_BLOCK;
    *event = (h2_pal_touch_event_t){.kind = f->pressed ? H2_PAL_TOUCH_EVENT_UP
                                                       : H2_PAL_TOUCH_EVENT_DOWN,
                                    .x = 180, .y = 290};
    f->pressed = !f->pressed;
    if (!f->pressed)
      f->retried_stop = true;
    f->next_edge = now(f) + 100u;
    return H2_PAL_OK;
  }
  if (!f->pressed) {
    if (f->step == 1 && f->touch_errors == 2 && !f->retry_cancelled_click)
      assert(f->starts ==
             0); // A failed read must not synthesize a start click.
    if (!tap_ready(f->step))
      return H2_PAL_ERR_WOULD_BLOCK;
    // Wait until the previous click actually completed through LVGL/controller.
    unsigned local = f->step % 3;
    h2_iperf_server_app_mode_t mode =
        f->step < 3   ? H2_IPERF_SERVER_APP_MODE_IPV4
        : f->step < 6 ? H2_IPERF_SERVER_APP_MODE_IPV6
                      : H2_IPERF_SERVER_APP_MODE_DUAL;
    if (local == 1 && snapshot.mode != mode)
      return H2_PAL_ERR_WOULD_BLOCK;
    if (local == 2 && snapshot.phase != H2_IPERF_SERVER_APP_LISTENING)
      return H2_PAL_ERR_WOULD_BLOCK;
    if (local == 0 && snapshot.phase != H2_IPERF_SERVER_APP_STOPPED)
      return H2_PAL_ERR_WOULD_BLOCK;
  }
  *event = (h2_pal_touch_event_t){.kind = f->pressed ? H2_PAL_TOUCH_EVENT_UP
                                                     : H2_PAL_TOUCH_EVENT_DOWN,
                                  .x = taps[f->step][0],
                                  .y = taps[f->step][1]};
  f->pressed = !f->pressed;
  if (!f->pressed) {
    if (f->retry_cancelled_click)
      f->retry_cancelled_click = false;
    else
      ++f->step;
  }
  f->next_edge = now(f) + 100;
  return H2_PAL_OK;
}
static int touch_close(void *user) {
  ++((fixture_t *)user)->touch_close;
  return H2_PAL_OK;
}
static int ui_ready(void *user) {
  fixture_t *f = user;
  f->ready = true;
  // Save production LVGL pixels for visual inspection, independent of UI state.
  const char *directory = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (directory != NULL) {
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/iperf-server.ppm", directory) > 0);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fputs("P6\n368 448\n255\n", file);
    for (unsigned i = 0; i < 368 * 448; ++i) {
      uint16_t pixel = framebuffer[i];
      uint8_t rgb[3] = {(uint8_t)(((pixel >> 11) & 31) * 255 / 31),
                        (uint8_t)(((pixel >> 5) & 63) * 255 / 63),
                        (uint8_t)((pixel & 31) * 255 / 31)};
      assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(fclose(file) == 0);
  }
  return H2_PAL_OK;
}
static bool finished(void *user) {
  fixture_t *f = user;
  if (now(f) > f->deadline) {
    fprintf(stderr, "touch script stuck at step %u\n", f->step);
    assert(false);
  }
  if (f->step < 9)
    return false;
  h2_iperf_server_app_snapshot_t snapshot;
  assert(h2_iperf_server_app_snapshot(f->app, &snapshot) == H2_PAL_OK);
  return snapshot.phase == H2_IPERF_SERVER_APP_STOPPED;
}
int main(void) {
  h2_iperf_test_env_t env;
  h2_iperf_test_env_init(&env, false);
  fixture_t f = {.time = env.config.time};
  f.deadline = now(&f) + 60000;
  const h2_pal_display_vtable_t display_vtable = {.open = display_open,
                                                  .get_info = display_info,
                                                  .draw_bitmap = draw,
                                                  .present = present,
                                                  .close = display_close};
  const h2_pal_display_api_t display = {.user = &f, .vtable = &display_vtable};
  const h2_pal_touch_vtable_t touch_vtable = {.open = touch_open,
                                              .get_info = touch_info,
                                              .poll_event = touch_poll,
                                              .close = touch_close};
  const h2_pal_touch_api_t touch = {.user = &f, .vtable = &touch_vtable};
  h2_runtime_t runtime = {.mem = env.config.mem,
                          .net = env.config.net,
                          .time = env.config.time,
                          .crypto = env.config.crypto,
                          .log = env.config.log,
                          .task = h2_desktop_platform_task_api(),
                          .sync = h2_desktop_platform_sync_api(),
                          .queue = h2_desktop_platform_queue_api(),
                          .display = &display,
                          .touch = &touch};
  const h2_iperf_server_app_config_t config = {
      .network_user = &f,
      .network_start = network_start,
      .network_stop = network_stop,
      .ui_ready = ui_ready,
      .ui_user = &f,
      .port = h2_iperf_test_free_port(env.config.net)};
  assert(h2_iperf_server_app_create(&runtime, &config, &f.app) == H2_PAL_OK);
  assert(h2_iperf_server_app_run_ui(f.app, finished, &f) == H2_PAL_OK);
  assert(h2_iperf_server_app_destroy(&f.app) == H2_PAL_OK);
  assert(f.ready && f.step == 9 && f.starts == 3 && f.stops == 3);
  assert(f.retried_stop && f.stop_attempts == 4u);
  assert(f.display_open == 1 && f.display_close == 1 && f.touch_open == 1 &&
         f.touch_close == 1);
  f.ready = false;
  f.bad_viewport = true;
  assert(h2_iperf_server_app_create(&runtime, &config, &f.app) == H2_PAL_OK);
  assert(h2_iperf_server_app_run_ui(f.app, finished, &f) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(!f.ready);
  assert(h2_iperf_server_app_destroy(&f.app) == H2_PAL_OK);
  f.bad_viewport = false;
  f.fail_draw = true;
  f.step = 0;
  assert(h2_iperf_server_app_create(&runtime, &config, &f.app) == H2_PAL_OK);
  assert(h2_iperf_server_app_run_ui(f.app, finished, &f) == H2_PAL_ERR_IO);
  assert(!f.ready);
  assert(h2_iperf_server_app_destroy(&f.app) == H2_PAL_OK);
  assert(f.display_open == 3 && f.display_close == 3 && f.touch_open == 3 &&
         f.touch_close == 3);
  f.fail_draw = false;
  f.fail_touch = true;
  assert(h2_iperf_server_app_create(&runtime, &config, &f.app) == H2_PAL_OK);
  assert(h2_iperf_server_app_run_ui(f.app, finished, &f) == H2_PAL_ERR_IO);
  assert(!f.ready);
  assert(h2_iperf_server_app_destroy(&f.app) == H2_PAL_OK);
  assert(f.display_open == 4 && f.display_close == 4 && f.touch_open == 4 &&
         f.touch_close == 4);
  h2_iperf_test_env_deinit(&env);
  puts("H2_IPERF_SERVER_UI_PASS touch-clicks=10 modes=3 display-closed=1 "
       "touch-closed=1 failed-ui-unconfirmed=3 touch-faults-recovered=2 "
       "cancelled-click=1 failed-stop-retry=1");
  return 0;
}
