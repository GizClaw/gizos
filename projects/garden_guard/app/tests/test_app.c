/* Run loop tests through the public entry with fake Display, Touch and Pref
 * providers and a Desktop-backed Runtime. */
#include "h2_desktop_platform.h"
#include "h2_garden_guard.h"
#include "h2_garden_guard_game.h"
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
#define HEIGHT 320
#define MAX_PIXELS (480 * 800)
#define SCRIPT_MAX 48

typedef struct fake_display {
  h2_display_info_t info;
  int open_result;
  int opened, open_calls, close_calls;
  size_t draws, presents, rows_drawn, pixels_drawn, full_frames;
  uint32_t brightness;
  uint16_t shadow[MAX_PIXELS];
} fake_display_t;

typedef struct fake_touch {
  h2_pal_touch_info_t info;
  int opened, open_calls, close_calls;
  h2_pal_result_t poll_error; /* Nonzero: every poll fails with it. */
  /* Scripted events delivered one per frame from touch_from_frame on. */
  h2_pal_touch_event_t script[SCRIPT_MAX];
  size_t script_count, script_next;
  unsigned last_frame;
} fake_touch_t;

typedef struct fake_pref {
  bool has_best;
  uint32_t best;
  unsigned opens, reads;
  char last_namespace[32];
  h2_pal_pref_namespace_t ns;
} fake_pref_t;

typedef struct probe {
  unsigned frame;
  float x, y;
  uint16_t value;
} probe_t;

typedef struct fixture {
  fake_display_t display;
  fake_touch_t touch;
  fake_pref_t pref;
  h2_pal_display_api_t display_api;
  h2_pal_touch_api_t touch_api;
  h2_pal_pref_api_t pref_api;
  h2_pal_mem_api_t mem_api;
  size_t allocations, frees;
  unsigned frames, exit_after, ready_calls;
  h2_pal_result_t ready_result;
  unsigned touch_from_frame;
  probe_t probes[2];
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
    memcpy(&d->shadow[(size_t)(rect->y + y) * (size_t)d->info.width +
                      (size_t)rect->x],
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
  /* One event per frame keeps gestures spread over game time. */
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

static int pref_close(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  return H2_PAL_OK;
}
static int pref_get_u32(h2_pal_pref_namespace_t* ns, const char* key,
                        uint32_t* out) {
  fake_pref_t* p = ns->user;
  ++p->reads;
  if (strcmp(key, H2_GARDEN_GUARD_PREF_BEST_KEY) != 0 || !p->has_best)
    return H2_PAL_ERR_NOT_FOUND;
  *out = p->best;
  return H2_PAL_OK;
}
static int pref_open(void* user, const char* name_space,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)mode;
  fake_pref_t* p = user;
  ++p->opens;
  (void)snprintf(p->last_namespace, sizeof(p->last_namespace), "%s",
                 name_space);
  memset(&p->ns, 0, sizeof(p->ns));
  p->ns.user = p;
  p->ns.close = pref_close;
  p->ns.get_u32 = pref_get_u32;
  *out = &p->ns;
  return H2_PAL_OK;
}
static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};

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

static uint16_t shadow_at(float x, float y) {
  return fx.display
      .shadow[(size_t)y * (size_t)fx.display.info.width + (size_t)x];
}

static bool should_exit(void* user) {
  fixture_t* f = user;
  for (unsigned i = 0; i < 2u; ++i)
    if (f->probes[i].frame == f->frames)
      f->probes[i].value = shadow_at(f->probes[i].x, f->probes[i].y);
  return f->frames++ >= f->exit_after;
}

static h2_pal_result_t ready(void* user) {
  fixture_t* f = user;
  ++f->ready_calls;
  /* The first frame is complete and visible before readiness. */
  CHECK(f->display.presents == 1u);
  CHECK(f->display.rows_drawn == (size_t)f->display.info.height);
  return f->ready_result;
}

static void reset(unsigned exit_after, int32_t width, int32_t height) {
  memset(&fx, 0, sizeof(fx));
  fx.display.info = (h2_display_info_t){width, height, H2_DISPLAY_PIXEL_RGB565};
  fx.touch.info = (h2_pal_touch_info_t){(uint32_t)width, (uint32_t)height};
  fx.touch.last_frame = UINT32_MAX;
  fx.display_api = (h2_pal_display_api_t){&fx.display, &display_vtable};
  fx.touch_api = (h2_pal_touch_api_t){&fx.touch, &touch_vtable};
  fx.pref_api = (h2_pal_pref_api_t){&fx.pref, &pref_vtable};
  fx.mem_api = (h2_pal_mem_api_t){NULL, &mem_vtable};
  fx.exit_after = exit_after;
  fx.touch_from_frame = UINT32_MAX;
  fx.probes[0].frame = fx.probes[1].frame = UINT32_MAX;
}

static h2_pal_result_t run(void) {
  h2_runtime_config_t config = {0};
  config.board = "garden-guard-test";
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
  config.pref = &fx.pref_api;
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
  const h2_garden_guard_config_t game = {.seed = 42u,
                                         .brightness_percent = 80u,
                                         .frame_period_ms = 10u,
                                         .ready = ready,
                                         .should_exit = should_exit,
                                         .user = &fx};
  const h2_pal_result_t rc = h2_garden_guard_run(runtime, &game);
  CHECK(h2_runtime_destroy(&runtime) == H2_PAL_OK);
  CHECK(fx.allocations == fx.frees);
  CHECK(fx.display.opened == 0 && fx.touch.opened == 0);
  return rc;
}

static void test_idle_title_pushes_once(void) {
  reset(30u, WIDTH, HEIGHT);
  fx.pref.has_best = true;
  fx.pref.best = 77u;
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.ready_calls == 1u);
  CHECK(fx.display.brightness == 80u);
  CHECK(fx.display.open_calls == 1 && fx.display.close_calls == 1);
  CHECK(fx.touch.open_calls == 1 && fx.touch.close_calls == 1);
  /* The best score is read from the App's own Pref namespace. */
  CHECK(fx.pref.opens >= 1u && fx.pref.reads >= 1u);
  CHECK(strcmp(fx.pref.last_namespace, H2_GARDEN_GUARD_PREF_NAMESPACE) == 0);
  /* The title is static: only the first frame is pushed. */
  CHECK(fx.display.full_frames == 1u && fx.display.presents == 1u);
  CHECK(fx.display.rows_drawn == HEIGHT);
}

/* Push a tap at display coordinates, scaled to the Touch viewport. */
static void script_tap(float x, float y) {
  CHECK(fx.touch.script_count + 2u <= SCRIPT_MAX);
  const float sx = (float)fx.touch.info.width / (float)fx.display.info.width;
  const float sy = (float)fx.touch.info.height / (float)fx.display.info.height;
  const int32_t tx = (int32_t)(x * sx), ty = (int32_t)(y * sy);
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, tx, ty};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, tx, ty};
}

static void test_touch_starts_and_plants(void) {
  reset(24u, WIDTH, HEIGHT);
  /* Touch reports half the display resolution. */
  fx.touch.info = (h2_pal_touch_info_t){WIDTH / 2, HEIGHT / 2};
  fx.touch_from_frame = 5u;
  h2_garden_guard_game_t layout_game;
  h2_garden_guard_game_init(&layout_game, WIDTH, HEIGHT, 1u, 0u);
  const h2_garden_guard_layout_t* l = &layout_game.layout;
  float cell_x, cell_y;
  h2_garden_guard_field_point(l, 0u, 1.5f, &cell_x, &cell_y);
  script_tap((float)WIDTH * 0.5f, (float)HEIGHT * 0.5f);
  script_tap(l->card_x[H2_GARDEN_GUARD_CARD_SHOOTER] + l->card_w * 0.5f,
             l->card_y + l->card_h * 0.5f);
  script_tap(cell_x, cell_y);
  /* The title panel's center between the artwork, then the empty cell. */
  fx.probes[0] = (probe_t){.frame = 3u,
                           .x = l->field_x + l->field_w * 0.5f,
                           .y = l->field_y + l->field_h * 0.5f};
  fx.probes[1] = (probe_t){.frame = 8u, .x = cell_x, .y = cell_y};
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  /* The title gave way to the lawn, and the shooter now stands in the cell. */
  CHECK(shadow_at(fx.probes[0].x, fx.probes[0].y) != fx.probes[0].value);
  CHECK(shadow_at(cell_x, cell_y) != fx.probes[1].value);
  CHECK(fx.display.full_frames == 1u);
}

static void test_shovel_restores_raised_figure(void) {
  reset(65u, WIDTH, HEIGHT);
  fx.touch.info = (h2_pal_touch_info_t){WIDTH / 2, HEIGHT / 2};
  fx.touch_from_frame = 5u;
  h2_garden_guard_game_t layout_game;
  h2_garden_guard_game_init(&layout_game, WIDTH, HEIGHT, 1u, 0u);
  const h2_garden_guard_layout_t* l = &layout_game.layout;
  float x, y;
  h2_garden_guard_field_point(l, 4u, 0.5f, &x, &y);
  const float head_y = y - 0.75f * h2_garden_guard_lane_size(l, 4u);
  script_tap(240.0f, 160.0f);
  script_tap(l->card_x[H2_GARDEN_GUARD_CARD_SUNFLOWER] + l->card_w * 0.5f,
             l->card_y + l->card_h * 0.5f);
  script_tap(x, y);
  for (unsigned i = 0; i < 15u; ++i) script_tap(470.0f, 20.0f);
  script_tap(l->card_x[H2_GARDEN_GUARD_CARD_SHOVEL] + l->card_w * 0.5f,
             l->card_y + l->card_h * 0.5f);
  script_tap(x, head_y);
  fx.probes[0] = (probe_t){.frame = 8u, .x = x, .y = head_y};
  fx.probes[1] = (probe_t){.frame = 42u, .x = x, .y = head_y};
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  CHECK(fx.probes[0].value != fx.probes[1].value);
  CHECK(shadow_at(x, head_y) == fx.probes[0].value);
  CHECK(fx.display.full_frames == 1u);
  CHECK(fx.display.pixels_drawn < (size_t)WIDTH * HEIGHT * fx.frames / 2u);
}

static void test_portrait_display(void) {
  reset(6u, 480, 800);
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.display.full_frames == 1u);
  CHECK(fx.display.rows_drawn == 800u);
}

static void test_display_open_failure(void) {
  reset(10u, WIDTH, HEIGHT);
  fx.display.open_result = H2_PAL_ERR_IO;
  CHECK(run() == H2_PAL_ERR_IO);
  CHECK(fx.touch.open_calls == 0 && fx.display.close_calls == 0);
  CHECK(fx.ready_calls == 0u);
}

static void test_unsupported_display(void) {
  reset(10u, WIDTH, HEIGHT);
  fx.display.info.native_format = H2_DISPLAY_PIXEL_RGB888;
  CHECK(run() == H2_PAL_ERR_UNSUPPORTED);
  reset(10u, WIDTH, HEIGHT);
  fx.display.info.height = 100;
  CHECK(run() == H2_PAL_ERR_UNSUPPORTED);
  CHECK(fx.display.close_calls == 1 && fx.touch.close_calls == 1);
}

static void test_persistent_touch_failure(void) {
  reset(1000u, WIDTH, HEIGHT);
  fx.touch.poll_error = H2_PAL_ERR_IO;
  CHECK(run() == H2_PAL_ERR_IO);
  CHECK(fx.frames == 30u);
  CHECK(fx.touch.close_calls == 1);
}

static void test_ready_failure_stops(void) {
  reset(100u, WIDTH, HEIGHT);
  fx.ready_result = H2_PAL_ERR_BUSY;
  CHECK(run() == H2_PAL_ERR_BUSY);
  CHECK(fx.ready_calls == 1u && fx.display.presents == 1u);
}

int main(void) {
  CHECK(h2_garden_guard_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  test_idle_title_pushes_once();
  test_touch_starts_and_plants();
  test_shovel_restores_raised_figure();
  test_portrait_display();
  test_display_open_failure();
  test_unsupported_display();
  test_persistent_touch_failure();
  test_ready_failure_stops();
  puts("garden guard app tests passed");
  return 0;
}
