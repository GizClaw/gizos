/* Run loop tests through the public entry with fake Display/Touch providers
 * and a Desktop-backed Runtime. */
#include "h2_desktop_platform.h"
#include "h2_fruit_slice.h"
#include "h2_pal_unsupported.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                               \
  do {                                                                 \
    if (!(condition)) {                                                \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
              #condition);                                             \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

#define WIDTH 480
#define HEIGHT 480
#define SCRIPT_MAX 32

typedef struct fake_display {
  h2_display_info_t info;
  int open_result;
  int opened, open_calls, close_calls;
  size_t draws, presents, rows_drawn, pixels_drawn, full_frames;
  uint32_t brightness;
  uint16_t shadow[WIDTH * HEIGHT];
} fake_display_t;

typedef struct fake_touch {
  h2_pal_touch_info_t info;
  int opened, open_calls, close_calls;
  h2_pal_result_t poll_error; /* Nonzero: every poll fails with it. */
  /* Scripted events delivered one per poll from frame first_frame on. */
  h2_pal_touch_event_t script[SCRIPT_MAX];
  size_t script_count, script_next;
  unsigned last_frame;
} fake_touch_t;

typedef struct fixture {
  fake_display_t display;
  fake_touch_t touch;
  h2_pal_display_api_t display_api;
  h2_pal_touch_api_t touch_api;
  h2_pal_mem_api_t mem_api;
  size_t allocations, frees;
  unsigned frames, exit_after, ready_calls;
  h2_pal_result_t ready_result;
  unsigned touch_from_frame;
  uint16_t probe_before;
} fixture_t;

static fixture_t fx;

static int display_open(void* user) {
  fake_display_t* d = user;
  ++d->open_calls;
  if (d->open_result != H2_PAL_OK) return d->open_result;
  d->opened = 1;
  return H2_PAL_OK;
}
static int display_info(void* user, h2_display_info_t* out) {
  fake_display_t* d = user;
  if (!d->opened) return H2_PAL_ERR_INVALID_STATE;
  *out = d->info;
  return H2_PAL_OK;
}
static int display_draw(void* user, const h2_display_rect_t* rect,
                        const void* pixels, size_t stride,
                        h2_display_pixel_format_t format) {
  fake_display_t* d = user;
  CHECK(d->opened);
  CHECK(format == H2_DISPLAY_PIXEL_RGB565);
  /* Partial rectangles start and end on even columns. */
  CHECK(rect->x >= 0 && rect->width > 0 && rect->x % 2 == 0 &&
        (rect->x + rect->width) % 2 == 0 &&
        rect->x + rect->width <= d->info.width);
  CHECK(rect->y >= 0 && rect->y + rect->height <= d->info.height);
  CHECK(stride == (size_t)d->info.width * 2u);
  for (int y = 0; y < rect->height; ++y)
    memcpy(&d->shadow[(size_t)(rect->y + y) * WIDTH + (size_t)rect->x],
           (const uint8_t*)pixels + (size_t)y * stride,
           (size_t)rect->width * 2u);
  ++d->draws;
  d->rows_drawn += (size_t)rect->height;
  d->pixels_drawn += (size_t)rect->width * (size_t)rect->height;
  if (rect->height == d->info.height && rect->width == d->info.width)
    ++d->full_frames;
  return H2_PAL_OK;
}
static int display_present(void* user) {
  fake_display_t* d = user;
  CHECK(d->opened);
  ++d->presents;
  return H2_PAL_OK;
}
static int display_brightness(void* user, uint32_t percent) {
  fake_display_t* d = user;
  d->brightness = percent;
  return H2_PAL_OK;
}
static int display_close(void* user) {
  fake_display_t* d = user;
  ++d->close_calls;
  d->opened = 0;
  return H2_PAL_OK;
}
static const h2_pal_display_vtable_t display_vtable = {
    .open = display_open,
    .get_info = display_info,
    .draw_bitmap = display_draw,
    .present = display_present,
    .set_brightness_percent = display_brightness,
    .close = display_close};

static h2_pal_result_t touch_open(void* user) {
  fake_touch_t* t = user;
  ++t->open_calls;
  t->opened = 1;
  return H2_PAL_OK;
}
static h2_pal_result_t touch_info(void* user, h2_pal_touch_info_t* out) {
  fake_touch_t* t = user;
  if (!t->opened) return H2_PAL_ERR_INVALID_STATE;
  *out = t->info;
  return H2_PAL_OK;
}
static h2_pal_result_t touch_poll(void* user, h2_pal_touch_event_t* out) {
  fake_touch_t* t = user;
  CHECK(t->opened);
  if (t->poll_error != H2_PAL_OK) return t->poll_error;
  /* One event per frame keeps strokes spread over game time. */
  if (fx.frames < fx.touch_from_frame || t->last_frame == fx.frames ||
      t->script_next == t->script_count)
    return H2_PAL_ERR_WOULD_BLOCK;
  t->last_frame = fx.frames;
  *out = t->script[t->script_next++];
  return H2_PAL_OK;
}
static h2_pal_result_t touch_close(void* user) {
  fake_touch_t* t = user;
  ++t->close_calls;
  t->opened = 0;
  return H2_PAL_OK;
}
static const h2_pal_touch_vtable_t touch_vtable = {.open = touch_open,
                                                   .get_info = touch_info,
                                                   .poll_event = touch_poll,
                                                   .close = touch_close};

static void* counted_alloc(void* user, size_t length) {
  (void)user;
  void* p = h2_pal_mem_alloc(h2_desktop_platform_default_allocator(), length);
  if (p != NULL) ++fx.allocations;
  return p;
}
static void counted_free(void* user, void* p) {
  (void)user;
  if (p != NULL) ++fx.frees;
  h2_pal_mem_free(h2_desktop_platform_default_allocator(), p);
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = counted_alloc,
                                               .free = counted_free};

static bool should_exit(void* user) {
  fixture_t* f = user;
  if (f->frames == 3u) f->probe_before = f->display.shadow[288u * WIDTH + 240u];
  return f->frames++ >= f->exit_after;
}

static h2_pal_result_t ready(void* user) {
  fixture_t* f = user;
  ++f->ready_calls;
  /* The first frame is complete and visible before readiness. */
  CHECK(f->display.presents == 1u);
  CHECK(f->display.rows_drawn == HEIGHT);
  return f->ready_result;
}

static void reset(unsigned exit_after) {
  memset(&fx, 0, sizeof(fx));
  fx.display.info = (h2_display_info_t){WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565};
  fx.touch.info = (h2_pal_touch_info_t){WIDTH, HEIGHT};
  fx.touch.last_frame = UINT32_MAX;
  fx.display_api = (h2_pal_display_api_t){&fx.display, &display_vtable};
  fx.touch_api = (h2_pal_touch_api_t){&fx.touch, &touch_vtable};
  fx.mem_api = (h2_pal_mem_api_t){NULL, &mem_vtable};
  fx.exit_after = exit_after;
  fx.touch_from_frame = UINT32_MAX;
}

static h2_pal_result_t run(void) {
  h2_runtime_config_t config = {0};
  config.board = "fruit-slice-test";
  config.target = "desktop";
  config.chip = "host";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = &fx.mem_api;
  config.log = h2_desktop_platform_log_api();
  config.time = h2_desktop_platform_time_api();
  config.timer = h2_pal_unsupported_timer_api();
  config.task = h2_desktop_platform_task_api();
  config.queue = h2_desktop_platform_queue_api();
  config.sync = h2_desktop_platform_sync_api();
  config.fs = h2_pal_unsupported_fs_api();
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = h2_pal_unsupported_pref_api();
  config.crypto = h2_pal_unsupported_crypto_api();
  config.http = h2_pal_unsupported_http_api();
  config.net = h2_pal_unsupported_net_api();
  config.netif = h2_pal_unsupported_netif_api();
  config.mqtt = h2_pal_unsupported_mqtt_api();
  config.webrtc = h2_pal_unsupported_webrtc_api();
  config.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  config.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  config.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  config.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  config.ble_host = h2_pal_unsupported_ble_host_api();
  config.modem = h2_pal_unsupported_modem_api();
  config.power = h2_pal_unsupported_power_api();
  config.camera = h2_pal_unsupported_camera_api();
  config.display = &fx.display_api;
  config.audio = h2_pal_unsupported_audio_api();
  config.audio_decoder = h2_pal_unsupported_audio_decoder_api();
  config.periph = h2_pal_unsupported_periph_api();
  config.button = h2_pal_unsupported_button_api();
  config.touch = &fx.touch_api;
  config.buzzer = h2_pal_unsupported_buzzer_api();
  config.nfc = h2_pal_unsupported_nfc_api();
  config.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
  config.imu = h2_pal_unsupported_imu_api();
  config.gpio_irq = h2_pal_unsupported_gpio_irq_api();
  config.led = h2_pal_unsupported_led_api();
  config.switch_api = h2_pal_unsupported_switch_api();
  config.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  config.input = h2_pal_unsupported_input_api();
  config.system_event = h2_pal_unsupported_system_event_api();
  config.video_decoder = h2_pal_unsupported_video_decoder_api();
  h2_runtime_t* runtime = NULL;
  CHECK(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  const h2_fruit_slice_config_t game = {.seed = 42u,
                                        .brightness_percent = 80u,
                                        .frame_period_ms = 10u,
                                        .ready = ready,
                                        .should_exit = should_exit,
                                        .user = &fx};
  const h2_pal_result_t rc = h2_fruit_slice_run(runtime, &game);
  CHECK(h2_runtime_destroy(&runtime) == H2_PAL_OK);
  CHECK(fx.allocations == fx.frees);
  CHECK(fx.display.opened == 0 && fx.touch.opened == 0);
  return rc;
}

static void test_runs_and_pushes_dirty_rows(void) {
  reset(40u);
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.ready_calls == 1u);
  CHECK(fx.display.brightness == 80u);
  CHECK(fx.display.presents == 40u);
  CHECK(fx.display.open_calls == 1 && fx.display.close_calls == 1);
  CHECK(fx.touch.open_calls == 1 && fx.touch.close_calls == 1);
  /* Session, three frame buffers, the texture cache and its textures. */
  CHECK(fx.allocations > 5u);
  /* Only the first frame is pushed in full. On the title screen later frames
   * push just the spinning melon's bands, never the static title text. */
  CHECK(fx.display.full_frames == 1u);
  CHECK(fx.display.rows_drawn - HEIGHT <= 39u * 12u * 16u);
  /* Only the melon's columns are pushed after the first frame. */
  CHECK(fx.display.pixels_drawn - (size_t)WIDTH * HEIGHT <=
        39u * 12u * 16u * 180u);
  CHECK(fx.display.shadow[0] != fx.display.shadow[288u * WIDTH + 240u]);
}

static void test_scaled_touch_slices_title_melon(void) {
  reset(70u);
  /* Touch reports half the display resolution. */
  fx.touch.info = (h2_pal_touch_info_t){WIDTH / 2, HEIGHT / 2};
  fx.touch_from_frame = 5u;
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 30, 144};
  for (int x = 60; x <= 210; x += 30)
    fx.touch.script[fx.touch.script_count++] =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, x, 144};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 210, 144};
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  /* The melon at the display center is gone once its halves fell away. */
  CHECK(fx.display.shadow[288u * WIDTH + 240u] != fx.probe_before);
}

static void test_display_open_failure(void) {
  reset(10u);
  fx.display.open_result = H2_PAL_ERR_IO;
  CHECK(run() == H2_PAL_ERR_IO);
  CHECK(fx.touch.open_calls == 0 && fx.display.close_calls == 0);
  CHECK(fx.ready_calls == 0u);
}

static void test_unsupported_display(void) {
  reset(10u);
  fx.display.info.native_format = H2_DISPLAY_PIXEL_RGB888;
  CHECK(run() == H2_PAL_ERR_UNSUPPORTED);
  reset(10u);
  fx.display.info.height = 100;
  CHECK(run() == H2_PAL_ERR_UNSUPPORTED);
  CHECK(fx.display.close_calls == 1 && fx.touch.close_calls == 1);
}

static void test_persistent_touch_failure(void) {
  reset(1000u);
  fx.touch.poll_error = H2_PAL_ERR_IO;
  CHECK(run() == H2_PAL_ERR_IO);
  CHECK(fx.frames == 30u);
  CHECK(fx.touch.close_calls == 1);
}

static void test_ready_failure_stops(void) {
  reset(100u);
  fx.ready_result = H2_PAL_ERR_BUSY;
  CHECK(run() == H2_PAL_ERR_BUSY);
  CHECK(fx.ready_calls == 1u && fx.display.presents == 1u);
}

int main(void) {
  CHECK(h2_fruit_slice_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  test_runs_and_pushes_dirty_rows();
  test_scaled_touch_slices_title_melon();
  test_display_open_failure();
  test_unsupported_display();
  test_persistent_touch_failure();
  test_ready_failure_stops();
  puts("fruit slice app tests passed");
  return 0;
}
