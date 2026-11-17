/* Run loop tests through the public entry with fake Display/Touch providers
 * and a Desktop-backed Runtime. */
#include "h2_desktop_platform.h"
#include "h2_pal_unsupported.h"
#include "h2_xianxia_rpg.h"

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

#define WIDTH 640
#define HEIGHT 480
#define SCRIPT_MAX 16

typedef struct fake_display {
  h2_display_info_t info;
  int opened, open_calls, close_calls;
  size_t draws, presents, full_frames;
  uint16_t shadow[WIDTH * HEIGHT];
} fake_display_t;

typedef struct fake_touch {
  h2_pal_touch_info_t info;
  int opened, open_calls, close_calls;
  h2_pal_touch_event_t script[SCRIPT_MAX];
  unsigned script_frame[SCRIPT_MAX];
  size_t script_count, script_next;
} fake_touch_t;

typedef struct fixture {
  fake_display_t display;
  fake_touch_t touch;
  h2_pal_display_api_t display_api;
  h2_pal_touch_api_t touch_api;
  unsigned frames, exit_after, ready_calls;
  h2_pal_result_t ready_result;
} fixture_t;

static fixture_t fx;

static int display_open(void* user) {
  fake_display_t* d = user;
  ++d->open_calls;
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
  CHECK(rect->x >= 0 && rect->width > 0 && rect->x % 2 == 0 &&
        (rect->x + rect->width) % 2 == 0 &&
        rect->x + rect->width <= d->info.width);
  CHECK(rect->y >= 0 && rect->height > 0 &&
        rect->y + rect->height <= d->info.height);
  CHECK(stride == (size_t)d->info.width * 2u);
  for (int y = 0; y < rect->height; ++y)
    memcpy(&d->shadow[(size_t)(rect->y + y) * WIDTH + (size_t)rect->x],
           (const uint8_t*)pixels + (size_t)y * stride,
           (size_t)rect->width * 2u);
  ++d->draws;
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
  (void)user;
  (void)percent;
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
  if (t->script_next == t->script_count ||
      fx.frames < t->script_frame[t->script_next])
    return H2_PAL_ERR_WOULD_BLOCK;
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

static bool should_exit(void* user) {
  fixture_t* f = user;
  return f->frames++ >= f->exit_after;
}

static h2_pal_result_t ready(void* user) {
  fixture_t* f = user;
  ++f->ready_calls;
  return f->ready_result;
}

static void script_tap(unsigned frame, int32_t x, int32_t y) {
  fake_touch_t* t = &fx.touch;
  CHECK(t->script_count + 2u <= SCRIPT_MAX);
  t->script[t->script_count] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, x, y};
  t->script_frame[t->script_count++] = frame;
  t->script[t->script_count] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, x, y};
  t->script_frame[t->script_count++] = frame;
}

static void reset(int32_t width, int32_t height,
                  h2_display_pixel_format_t format) {
  memset(&fx, 0, sizeof(fx));
  fx.display.info.width = width;
  fx.display.info.height = height;
  fx.display.info.native_format = format;
  fx.touch.info.width = (uint32_t)width;
  fx.touch.info.height = (uint32_t)height;
  fx.display_api =
      (h2_pal_display_api_t){.user = &fx.display, .vtable = &display_vtable};
  fx.touch_api =
      (h2_pal_touch_api_t){.user = &fx.touch, .vtable = &touch_vtable};
}

static h2_runtime_config_t runtime_config(void) {
  h2_runtime_config_t config = {0};
  config.board = "test";
  config.target = "test";
  config.chip = "host";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = h2_desktop_platform_default_allocator();
  config.log = h2_desktop_platform_log_api();
  config.time = h2_desktop_platform_time_api();
  config.timer = h2_desktop_platform_timer_api();
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
  return config;
}

static h2_pal_result_t run_once(void) {
  const h2_runtime_config_t config = runtime_config();
  h2_runtime_t* runtime = NULL;
  CHECK(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  const h2_xianxia_rpg_config_t game = {.seed = 42u,
                                        .frame_period_ms = 10u,
                                        .ready = ready,
                                        .should_exit = should_exit,
                                        .user = &fx};
  const h2_pal_result_t rc = h2_xianxia_rpg_run(runtime, &game);
  CHECK(h2_runtime_destroy(&runtime) == H2_PAL_OK);
  return rc;
}

static size_t distinct_colors(void) {
  size_t count = 0;
  uint16_t seen[64];
  for (size_t i = 0; i < (size_t)WIDTH * HEIGHT && count < 64u; i += 97u) {
    bool known = false;
    for (size_t j = 0; j < count; ++j)
      known = known || seen[j] == fx.display.shadow[i];
    if (!known) seen[count++] = fx.display.shadow[i];
  }
  return count;
}

static void test_plays_and_cleans_up(void) {
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 90u;
  /* "New journey" on the title, after the tap guard. */
  script_tap(40u, WIDTH / 2, HEIGHT * 60 / 100 + 24);
  CHECK(run_once() == H2_PAL_OK);
  CHECK(fx.display.open_calls == 1 && fx.display.close_calls == 1);
  CHECK(fx.touch.open_calls == 1 && fx.touch.close_calls == 1);
  CHECK(fx.ready_calls == 1u);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  /* The title and the first map each start with a full-screen push. */
  CHECK(fx.display.full_frames >= 2u);
  CHECK(fx.display.presents >= 2u);
  CHECK(distinct_colors() > 16u);
}

static void test_rejects_unusable_displays(void) {
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB888);
  CHECK(run_once() == H2_PAL_ERR_UNSUPPORTED);
  CHECK(fx.display.close_calls == 1 && fx.touch.close_calls == 1);
  CHECK(fx.ready_calls == 0u);
  reset(240, 200, H2_DISPLAY_PIXEL_RGB565);
  CHECK(run_once() == H2_PAL_ERR_UNSUPPORTED);
  CHECK(fx.display.draws == 0u);
}

static void test_ready_failure_ends_run(void) {
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 1000u;
  fx.ready_result = H2_PAL_ERR_IO;
  CHECK(run_once() == H2_PAL_ERR_IO);
  CHECK(fx.ready_calls == 1u);
  CHECK(fx.display.close_calls == 1 && fx.touch.close_calls == 1);
}

int main(void) {
  CHECK(h2_xianxia_rpg_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  test_plays_and_cleans_up();
  test_rejects_unusable_displays();
  test_ready_failure_ends_run();
  printf("xianxia_rpg app tests passed\n");
  return 0;
}
