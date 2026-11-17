/* Run loop tests through the public entry with fake Display/Touch providers
 * and a Desktop-backed Runtime. */
#include "h2_desktop_platform.h"
#include "h2_fruit_slice.h"
#include "h2_fruit_slice_game.h"
#include "h2_pal_unsupported.h"
#include "h2_sqlite.h"

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
  bool use_pref, fast_clock;
  unsigned autoplay_until;
  uint64_t clock_ms;
  uint32_t earned_fruit_credit;
  const h2_pal_pref_api_t* pref_override;
  bool use_audio, audio_active;
  unsigned audio_starts, audio_stops, audio_opens, audio_closes, audio_writes;
  unsigned pause_from, pause_until;
  int audio_close_result;
  unsigned audio_stop_failures;
  h2_pal_audio_api_t audio_api;
  h2_pal_audio_track_t audio_track;
} fixture_t;

static fixture_t fx;
static struct {
  uint8_t saved[H2_FRUIT_SLICE_PROGRESS_BYTES];
  uint8_t staged[H2_FRUIT_SLICE_PROGRESS_BYTES];
  bool exists, pending;
  unsigned commits, failed_commits, reads, best_reads;
  int read_result;
  h2_pal_pref_namespace_t ns;
} pref;

static int pref_get_blob(h2_pal_pref_namespace_t* ns,
                         const h2_pal_mem_api_t* allocator, const char* key,
                         void** out_data, size_t* out_size) {
  (void)ns;
  if (strcmp(key, "cards_v1") == 0) return H2_PAL_ERR_UNSUPPORTED;
  CHECK(strcmp(key, "progress_v1") == 0);
  ++pref.reads;
  if (pref.read_result != H2_PAL_OK) return pref.read_result;
  if (!pref.exists) return H2_PAL_ERR_NOT_FOUND;
  *out_size = sizeof(pref.saved);
  *out_data = h2_pal_mem_alloc(allocator, *out_size);
  if (*out_data == NULL) return H2_PAL_ERR_NO_MEMORY;
  memcpy(*out_data, pref.saved, *out_size);
  return H2_PAL_OK;
}
static int pref_set_blob(h2_pal_pref_namespace_t* ns, const char* key,
                         const void* data, size_t size) {
  (void)ns;
  CHECK(strcmp(key, "progress_v1") == 0 && size == sizeof(pref.staged));
  memcpy(pref.staged, data, size);
  pref.pending = true;
  return H2_PAL_OK;
}
static int pref_get_best(h2_pal_pref_namespace_t* ns, const char* key,
                         uint32_t* out_value) {
  (void)ns;
  if (strcmp(key, "audio_v1") == 0) return H2_PAL_ERR_NOT_FOUND;
  CHECK(strcmp(key, H2_FRUIT_SLICE_PREF_BEST_KEY) == 0);
  *out_value = 21u;
  ++pref.best_reads;
  return H2_PAL_OK;
}
static int pref_commit(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  ++pref.commits;
  if (pref.failed_commits != 0u) {
    --pref.failed_commits;
    return H2_PAL_ERR_IO;
  }
  CHECK(pref.pending);
  memcpy(pref.saved, pref.staged, sizeof(pref.saved));
  pref.exists = true;
  pref.pending = false;
  return H2_PAL_OK;
}
static int pref_close(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  pref.pending = false;
  return H2_PAL_OK;
}
static int pref_open(void* user, const char* name, h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)user;
  (void)mode;
  CHECK(strcmp(name, H2_FRUIT_SLICE_PREF_NAMESPACE) == 0);
  pref.ns = (h2_pal_pref_namespace_t){.get_blob = pref_get_blob,
                                      .set_blob = pref_set_blob,
                                      .get_u32 = pref_get_best,
                                      .commit = pref_commit,
                                      .close = pref_close};
  *out = &pref.ns;
  return H2_PAL_OK;
}
static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};
static const h2_pal_pref_api_t pref_api = {.vtable = &pref_vtable};

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
  /* Accelerated first-install regression: ordinary pointer strokes play
   * real rounds, then leave enough game time for misses to end the last run. */
  if (fx.autoplay_until != 0u && fx.frames >= 2u &&
      fx.frames < fx.autoplay_until && t->last_frame != fx.frames) {
    t->last_frame = fx.frames;
    const unsigned point = (fx.frames - 2u) % 150u;
    *out = (h2_pal_touch_event_t){
        point % 2u == 0u ? H2_PAL_TOUCH_EVENT_DOWN : H2_PAL_TOUCH_EVENT_UP,
        point % 2u == 0u ? 0 : WIDTH, point < 2u ? 346 : 280};
    return H2_PAL_OK;
  }
  /* One event per frame keeps strokes spread over game time. */
  if (fx.frames < fx.touch_from_frame || t->last_frame == fx.frames ||
      t->script_next == t->script_count)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (fx.autoplay_until != 0u && fx.frames == fx.touch_from_frame) {
    h2_pal_pref_namespace_t* ns = NULL;
    CHECK(h2_pal_pref_open(fx.pref_override, H2_FRUIT_SLICE_PREF_NAMESPACE,
                           H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
    void* data = NULL;
    size_t size = 0u;
    CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(),
                       "progress_v1", &data, &size) == H2_PAL_OK);
    h2_fruit_slice_progress_t earned = {0};
    CHECK(h2_fruit_slice_progress_decode(&earned, data, size));
    fx.earned_fruit_credit = earned.fruit_credit;
    h2_pal_mem_free(h2_desktop_platform_default_allocator(), data);
    CHECK(ns->close(ns) == H2_PAL_OK);
  }
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

static int audio_info(void* user, h2_audio_info_t* info) {
  (void)user;
  *info = (h2_audio_info_t){
      .available = 1,
      .playback_supported = 1,
      .playback_format = {16000, 320, 1, H2_AUDIO_SAMPLE_S16LE},
      .max_tracks = 1};
  return H2_PAL_OK;
}
static int audio_start(void* user) {
  ++((fixture_t*)user)->audio_starts;
  return H2_PAL_OK;
}
static int audio_stop(void* user) {
  fixture_t* f = user;
  CHECK(!f->audio_active);
  ++f->audio_stops;
  if (f->audio_stop_failures != 0u) {
    --f->audio_stop_failures;
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}
static int audio_write(h2_pal_audio_track_t* track,
                       const h2_audio_frame_t* frame, uint32_t timeout) {
  fixture_t* f = track->user;
  CHECK(f->audio_active && timeout == 0u && frame->bytes == 640u);
  CHECK(!(f->frames >= f->pause_from && f->frames < f->pause_until));
  ++f->audio_writes;
  return H2_PAL_OK;
}
static int audio_track_close(h2_pal_audio_track_t* track) {
  fixture_t* f = track->user;
  CHECK(f->audio_active);
  f->audio_active = false;
  ++f->audio_closes;
  return f->audio_close_result;
}
static int audio_create(void* user, const h2_audio_track_config_t* config,
                        h2_pal_audio_track_t** out) {
  fixture_t* f = user;
  CHECK(!f->audio_active && config->buffer_frames == 2u);
  f->audio_active = true;
  ++f->audio_opens;
  f->audio_track = (h2_pal_audio_track_t){
      .user = f, .write = audio_write, .close = audio_track_close};
  *out = &f->audio_track;
  return H2_PAL_OK;
}
static const h2_pal_audio_vtable_t audio_vtable = {
    .get_info = audio_info,
    .start_speaker = audio_start,
    .stop_speaker = audio_stop,
    .create_track = audio_create};

static bool is_paused(void* user) {
  fixture_t* f = user;
  if (f->pause_from != 0u && f->frames == f->pause_from + 1u)
    CHECK(!f->audio_active);
  return f->frames >= f->pause_from && f->frames < f->pause_until;
}

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
  fx.audio_api = (h2_pal_audio_api_t){&fx, &audio_vtable};
  fx.exit_after = exit_after;
  fx.touch_from_frame = UINT32_MAX;
}

static h2_pal_result_t accelerated_clock(void* user, uint64_t* out_ms) {
  (void)user;
  fx.clock_ms += 10u;
  *out_ms = fx.clock_ms;
  return H2_PAL_OK;
}

static h2_pal_result_t run(void) {
  const h2_pal_time_api_t* real_time = h2_desktop_platform_time_api();
  h2_pal_time_vtable_t time_vtable = *real_time->vtable;
  time_vtable.get_monotonic_ms = accelerated_clock;
  const h2_pal_time_api_t time_api = {.user = real_time->user,
                                      .vtable = &time_vtable};
  h2_runtime_config_t config = {0};
  config.board = "fruit-slice-test";
  config.target = "desktop";
  config.chip = "host";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = &fx.mem_api;
  config.log = h2_desktop_platform_log_api();
  config.time = fx.fast_clock ? &time_api : real_time;
  config.timer = h2_pal_unsupported_timer_api();
  config.task = h2_desktop_platform_task_api();
  config.queue = h2_desktop_platform_queue_api();
  config.sync = h2_desktop_platform_sync_api();
  config.fs = h2_pal_unsupported_fs_api();
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = fx.pref_override != NULL ? fx.pref_override
                : fx.use_pref            ? &pref_api
                                         : h2_pal_unsupported_pref_api();
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
  config.audio = fx.use_audio ? &fx.audio_api : h2_pal_unsupported_audio_api();
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
  const h2_fruit_slice_config_t game = {
      .seed = 42u,
      .brightness_percent = 80u,
      .frame_period_ms = fx.fast_clock ? 1u : 10u,
      .ready = ready,
      .should_exit = should_exit,
      .is_paused = is_paused,
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

static void script_tap(int32_t x, int32_t y) {
  CHECK(fx.touch.script_count + 2u <= SCRIPT_MAX);
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, x, y};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, x, y};
}

static void test_progress_persists_across_sessions(void) {
  memset(&pref, 0, sizeof(pref));
  const h2_fruit_slice_progress_t initial = {80u, 10u, 0u, 1u, 1u, 0u};
  h2_fruit_slice_progress_encode(&initial, pref.saved);
  pref.exists = true;
  pref.failed_commits = 1u;
  reset(25u);
  fx.use_pref = true;
  fx.touch_from_frame = 2u;
  script_tap(120, 450); /* Tasks */
  script_tap(396, 151); /* Claim fruit credit: first commit fails. */
  script_tap(396, 343); /* Claim score achievement: retry succeeds. */
  script_tap(240, 450); /* Back */
  script_tap(360, 450); /* Shop */
  script_tap(396, 343); /* Buy and equip axe. */
  CHECK(run() == H2_PAL_OK);
  CHECK(pref.commits == 3u && pref.best_reads == 1u && pref.reads == 1u);
  h2_fruit_slice_progress_t saved = {0};
  CHECK(h2_fruit_slice_progress_decode(&saved, pref.saved, sizeof(pref.saved)));
  CHECK(saved.coins == 45u && saved.owned == 5u && saved.equipped == 2u);
  CHECK(saved.fruit_credit == 0u && saved.score_task == 2u);
  reset(25u);
  fx.use_pref = true;
  fx.touch_from_frame = 2u;
  script_tap(120, 450);
  script_tap(396, 151); /* Cannot re-claim saved consumed credits. */
  script_tap(396, 343); /* Cannot re-claim one-shot achievement. */
  script_tap(240, 450);
  script_tap(360, 450);
  script_tap(396, 151); /* Equip classic. */
  script_tap(396, 343); /* Re-equip owned axe for free. */
  CHECK(run() == H2_PAL_OK);
  CHECK(h2_fruit_slice_progress_decode(&saved, pref.saved, sizeof(pref.saved)));
  CHECK(saved.coins == 45u && saved.owned == 5u && saved.equipped == 2u);
  CHECK(pref.commits == 5u && pref.best_reads == 2u && pref.reads == 2u);

  /* A final exit flush retries a transaction that failed in the last frame. */
  reset(8u);
  fx.use_pref = true;
  fx.touch_from_frame = 2u;
  pref.failed_commits = 1u;
  script_tap(360, 450);
  script_tap(396, 151);
  CHECK(run() == H2_PAL_OK);
  CHECK(pref.failed_commits == 0u && pref.commits == 7u);
  CHECK(h2_fruit_slice_progress_decode(&saved, pref.saved, sizeof(pref.saved)));
  CHECK(saved.equipped == 0u && saved.coins == 45u);
}

static void test_unreadable_progress_is_preserved(void) {
  memset(&pref, 0, sizeof(pref));
  const h2_fruit_slice_progress_t initial = {72u, 10u, 2u, 2u, 3u, 1u};
  h2_fruit_slice_progress_encode(&initial, pref.saved);
  pref.exists = true;
  for (unsigned failure = 0; failure < 2u; ++failure) {
    if (failure == 0u)
      pref.read_result = H2_PAL_ERR_IO;
    else {
      pref.read_result = H2_PAL_OK;
      pref.saved[4] = 2u; /* A newer format must never be overwritten. */
    }
    uint8_t before[H2_FRUIT_SLICE_PROGRESS_BYTES];
    memcpy(before, pref.saved, sizeof(before));
    reset(8u);
    fx.use_pref = true;
    fx.touch_from_frame = 2u;
    script_tap(120, 450);
    script_tap(396, 343); /* Session-only claim cannot replace unread data. */
    CHECK(run() == H2_PAL_OK);
    CHECK(pref.commits == 0u);
    CHECK(memcmp(before, pref.saved, sizeof(before)) == 0);
  }
  /* First launch with only the legacy best key can persist new rewards. */
  pref.exists = false;
  reset(8u);
  fx.use_pref = true;
  fx.touch_from_frame = 2u;
  script_tap(120, 450);
  script_tap(396, 343);
  CHECK(run() == H2_PAL_OK);
  h2_fruit_slice_progress_t saved = {0};
  CHECK(pref.commits == 1u && pref.best_reads == 3u);
  CHECK(h2_fruit_slice_progress_decode(&saved, pref.saved, sizeof(pref.saved)));
  CHECK(saved.coins == 40u && saved.owned == 1u && saved.score_task == 2u);
}

static void test_fresh_sqlite_storage(void) {
  const char* temporary = getenv("TEST_TMPDIR");
  CHECK(temporary != NULL);
  char database[512];
  const int length =
      snprintf(database, sizeof(database), "%s/first-launch.sqlite", temporary);
  CHECK(length > 0 && (size_t)length < sizeof(database));
  h2_sqlite_t* storage = NULL;
  const h2_sqlite_config_t config = {.path = database};
  CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
  /* Like mobile storage creation, provider creation has not created a DB. */
  h2_pal_pref_namespace_t* ns = NULL;
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_ONLY, &ns) != H2_PAL_OK);
  /* Accelerate time but keep production spawning, score/task transitions,
   * pointer input, save effects and rendering. Nothing seeds the profile. */
  reset(1525u);
  fx.pref_override = h2_sqlite_pref_api(storage);
  fx.use_audio = true;
  fx.fast_clock = true;
  fx.autoplay_until = 1202u;
  fx.touch_from_frame = 1505u;
  script_tap(120, 450);
  script_tap(396, 151);
  script_tap(396, 151);
  script_tap(396, 151); /* Three earned rewards = 45 coins. */
  script_tap(240, 450);
  script_tap(360, 450);
  script_tap(396, 247); /* Buy/equip katana for 40 coins. */
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  CHECK(fx.earned_fruit_credit >= 30u);
  CHECK(fx.audio_writes > 0u && fx.audio_opens == 1u && fx.audio_closes == 1u);
  const uint32_t remaining_credit = fx.earned_fruit_credit - 30u;
  void* data = NULL;
  size_t size = 0u;
  h2_sqlite_destroy(storage);
  CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
  CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(), "progress_v1",
                     &data, &size) == H2_PAL_OK);
  h2_fruit_slice_progress_t saved = {0};
  CHECK(h2_fruit_slice_progress_decode(&saved, data, size));
  CHECK(saved.coins == 5u && saved.fruit_credit == remaining_credit &&
        saved.owned == 3u && saved.equipped == 1u);
  h2_pal_mem_free(h2_desktop_platform_default_allocator(), data);
  CHECK(ns->close(ns) == H2_PAL_OK);
  /* A new App call must load that ownership/equipment: switching to classic
   * writes equipped=0 and leaves the wallet and consumed task credits alone. */
  reset(12u);
  fx.pref_override = h2_sqlite_pref_api(storage);
  fx.use_audio = true;
  fx.touch_from_frame = 2u;
  script_tap(340, 20); /* Persist SFX mute independently of progression. */
  script_tap(428, 20); /* Persist BGM mute. */
  script_tap(360, 450);
  script_tap(396, 151);
  CHECK(run() == H2_PAL_OK);
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
  CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(), "progress_v1",
                     &data, &size) == H2_PAL_OK);
  CHECK(h2_fruit_slice_progress_decode(&saved, data, size));
  CHECK(saved.coins == 5u && saved.fruit_credit == remaining_credit &&
        saved.owned == 3u && saved.equipped == 0u);
  uint32_t audio_preference = 0u;
  CHECK(ns->get_u32(ns, "audio_v1", &audio_preference) == H2_PAL_OK);
  CHECK((audio_preference & 3u) == 3u);
  h2_pal_mem_free(h2_desktop_platform_default_allocator(), data);
  CHECK(ns->set_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, 21u) == H2_PAL_OK);
  CHECK(ns->commit(ns) == H2_PAL_OK);
  CHECK(ns->close(ns) == H2_PAL_OK);

  /* The writable initialization must still preserve future and corrupt blobs
   * even when a session-only achievement claim makes the model dirty. */
  for (unsigned invalid = 0; invalid < 2u; ++invalid) {
    uint8_t bytes[H2_FRUIT_SLICE_PROGRESS_BYTES];
    h2_fruit_slice_progress_encode(&saved, bytes);
    bytes[invalid == 0u ? 4u : 0u] = invalid == 0u ? 2u : 0u;
    CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                           H2_FRUIT_SLICE_PREF_NAMESPACE,
                           H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
    CHECK(ns->set_blob(ns, "progress_v1", bytes, sizeof(bytes)) == H2_PAL_OK);
    CHECK(ns->commit(ns) == H2_PAL_OK);
    CHECK(ns->close(ns) == H2_PAL_OK);
    reset(8u);
    fx.pref_override = h2_sqlite_pref_api(storage);
    fx.touch_from_frame = 2u;
    script_tap(120, 450);
    script_tap(396, 343);
    CHECK(run() == H2_PAL_OK);
    CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                           H2_FRUIT_SLICE_PREF_NAMESPACE,
                           H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
    CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(),
                       "progress_v1", &data, &size) == H2_PAL_OK);
    CHECK(size == sizeof(bytes) && memcmp(data, bytes, sizeof(bytes)) == 0);
    h2_pal_mem_free(h2_desktop_platform_default_allocator(), data);
    CHECK(ns->close(ns) == H2_PAL_OK);
  }
  h2_sqlite_destroy(storage);
  CHECK(remove(database) == 0);
}

static void test_audio_gesture_pause_and_exit(void) {
  reset(40u);
  fx.use_audio = true;
  fx.pause_from = 20u;
  fx.pause_until = 24u;
  fx.touch_from_frame = 5u;
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 60, 288};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 400, 288};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 400, 288};
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.audio_writes > 0u);
  CHECK(fx.audio_opens == 2u && fx.audio_closes == 2u);
  CHECK(fx.audio_starts == 2u && fx.audio_stops == 2u);
  CHECK(!fx.audio_active);
}

static void test_audio_close_errors_release_runtime_wrapped_track(void) {
  for (unsigned failure = 0u; failure < 3u; ++failure) {
    reset(30u);
    fx.use_audio = true;
    fx.audio_close_result = failure < 2u ? H2_PAL_ERR_IO : H2_PAL_OK;
    fx.audio_stop_failures = failure == 2u ? 1u : 0u;
    if (failure == 0u) {
      fx.pause_from = 20u;
      fx.pause_until = 24u;
    }
    fx.touch_from_frame = 5u;
    fx.touch.script[fx.touch.script_count++] =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 60, 288};
    fx.touch.script[fx.touch.script_count++] =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 400, 288};
    fx.touch.script[fx.touch.script_count++] =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 400, 288};
    CHECK(run() == H2_PAL_ERR_IO);
    CHECK(fx.audio_opens == 1u && fx.audio_closes == 1u);
    CHECK(fx.audio_starts == 1u);
    CHECK(fx.audio_stops == (failure == 2u ? 2u : 1u));
    CHECK(!fx.audio_active);
  }
}

int main(void) {
  test_fresh_sqlite_storage();
  test_unreadable_progress_is_preserved();
  test_progress_persists_across_sessions();
  CHECK(h2_fruit_slice_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  test_runs_and_pushes_dirty_rows();
  test_scaled_touch_slices_title_melon();
  test_display_open_failure();
  test_unsupported_display();
  test_persistent_touch_failure();
  test_ready_failure_stops();
  test_audio_gesture_pause_and_exit();
  test_audio_close_errors_release_runtime_wrapped_track();
  puts("fruit slice app tests passed");
  return 0;
}
