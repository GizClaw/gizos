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
  unsigned script_frame[SCRIPT_MAX];
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
  unsigned autoplay_until, autoplay_pause_from, autoplay_pause_until;
  uint64_t clock_ms;
  uint32_t earned_fruit_credit;
  const char* capture_name;
  unsigned capture_every, capture_from, capture_until;
  const h2_pal_pref_api_t* pref_override;
  bool use_audio, audio_active;
  unsigned audio_starts, audio_stops, audio_opens, audio_closes, audio_writes;
  unsigned pause_from, pause_until;
  int audio_close_result;
  unsigned audio_stop_failures, audio_block_writes, audio_successful_writes;
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
  bool cards_enabled, cards_exists, cards_pending;
  uint8_t cards_saved[H2_FRUIT_SLICE_CARDS_BYTES],
      cards_staged[H2_FRUIT_SLICE_CARDS_BYTES];
  unsigned cards_commits, cards_failures, cards_reads;
  int cards_read_result;
  h2_pal_pref_namespace_t ns;
} pref;

static int pref_get_blob(h2_pal_pref_namespace_t* ns,
                         const h2_pal_mem_api_t* allocator, const char* key,
                         void** out_data, size_t* out_size) {
  (void)ns;
  if (strcmp(key, "cards_v1") == 0) {
    if (!pref.cards_enabled) return H2_PAL_ERR_UNSUPPORTED;
    ++pref.cards_reads;
    if (pref.cards_read_result != H2_PAL_OK) return pref.cards_read_result;
    if (!pref.cards_exists) return H2_PAL_ERR_NOT_FOUND;
    *out_size = sizeof(pref.cards_saved);
    *out_data = h2_pal_mem_alloc(allocator, *out_size);
    if (*out_data == NULL) return H2_PAL_ERR_NO_MEMORY;
    memcpy(*out_data, pref.cards_saved, *out_size);
    return H2_PAL_OK;
  }
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
  if (strcmp(key, "cards_v1") == 0) {
    CHECK(pref.cards_enabled && size == sizeof(pref.cards_staged));
    memcpy(pref.cards_staged, data, size);
    pref.cards_pending = true;
    return H2_PAL_OK;
  }
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
  if (pref.cards_pending) {
    ++pref.cards_commits;
    if (pref.cards_failures) {
      --pref.cards_failures;
      return H2_PAL_ERR_IO;
    }
    memcpy(pref.cards_saved, pref.cards_staged, sizeof(pref.cards_saved));
    pref.cards_exists = true;
    pref.cards_pending = false;
    return H2_PAL_OK;
  }
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
  pref.cards_pending = false;
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
      fx.frames < fx.autoplay_until && t->last_frame != fx.frames &&
      !(fx.frames >= fx.autoplay_pause_from &&
        fx.frames < fx.autoplay_pause_until)) {
    t->last_frame = fx.frames;
    const unsigned point = (fx.frames - 2u) % 150u;
    *out = (h2_pal_touch_event_t){
        point % 2u == 0u ? H2_PAL_TOUCH_EVENT_DOWN : H2_PAL_TOUCH_EVENT_UP,
        point % 2u == 0u ? 0 : WIDTH, point < 2u ? 346 : 280};
    return H2_PAL_OK;
  }
  /* Finish naturally earned reveals after autoplay stops, without slicing
   * the restart fruit or changing the economy controls that follow. */
  if (fx.autoplay_until != 0u && fx.frames >= fx.autoplay_until &&
      fx.frames < fx.touch_from_frame && t->last_frame != fx.frames) {
    t->last_frame = fx.frames;
    *out = (h2_pal_touch_event_t){
        fx.frames % 2u == 0u ? H2_PAL_TOUCH_EVENT_DOWN : H2_PAL_TOUCH_EVENT_UP,
        5, 100};
    return H2_PAL_OK;
  }
  /* One event per frame keeps strokes spread over game time. */
  if (fx.frames < fx.touch_from_frame || t->last_frame == fx.frames ||
      t->script_next == t->script_count ||
      fx.frames < t->script_frame[t->script_next])
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
  if (f->audio_block_writes != 0u) {
    --f->audio_block_writes;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  ++f->audio_successful_writes;
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

static void capture_frame(const fixture_t* f) {
  const char* directory = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (directory == NULL || f->capture_name == NULL) return;
  char path[512];
  (void)snprintf(path, sizeof(path), "%s/%s-%04u.ppm", directory,
                 f->capture_name, f->frames);
  FILE* file = fopen(path, "wb");
  CHECK(file != NULL);
  const int width = f->display.info.width, height = f->display.info.height;
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const uint16_t c = f->display.shadow[y * WIDTH + x];
      const unsigned char rgb[3] = {
          (unsigned char)((c >> 11) * 255u / 31u),
          (unsigned char)(((c >> 5) & 63u) * 255u / 63u),
          (unsigned char)((c & 31u) * 255u / 31u)};
      CHECK(fwrite(rgb, 1, 3, file) == 3u);
    }
  CHECK(fclose(file) == 0);
}

static bool should_exit(void* user) {
  fixture_t* f = user;
  if (f->frames == f->exit_after ||
      (f->capture_every && f->frames >= f->capture_from && f->frames &&
       f->frames <= f->capture_until && f->frames % f->capture_every == 0u))
    capture_frame(f);
  if (f->frames == 3u) f->probe_before = f->display.shadow[288u * WIDTH + 240u];
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

static void script_tap_at(unsigned frame, int32_t x, int32_t y) {
  const size_t next = fx.touch.script_count;
  script_tap(x, y);
  fx.touch.script_frame[next] = frame;
  fx.touch.script_frame[next + 1u] = frame + 1u;
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
  fx.capture_name = "actual-app-play-480";
  fx.autoplay_pause_from = 326u;
  fx.autoplay_pause_until = 375u;
  fx.capture_every = 4u;
  fx.capture_from = 324u;
  fx.capture_until = 380u;
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
  CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(), "cards_v1",
                     &data, &size) == H2_PAL_OK);
  h2_fruit_slice_cards_t collection;
  h2_fruit_slice_cards_init(&collection, 240, 240, 1u);
  CHECK(h2_fruit_slice_cards_decode(&collection, data, size));
  CHECK(h2_fruit_slice_cards_owned_total(&collection) >= 3u);
  CHECK(collection.counts[H2_FRUIT_SLICE_CARD_BOMB] > 0u);
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

  /* Exercise the real App album at the minimum target size, through Touch. */
  reset(100u);
  fx.display.info.width = fx.display.info.height = 240;
  fx.touch.info.width = fx.touch.info.height = 240;
  fx.pref_override = h2_sqlite_pref_api(storage);
  fx.fast_clock = true;
  fx.touch_from_frame = 2u;
  fx.capture_name = "actual-app-album-240";
  fx.capture_every = 20u;
  fx.capture_until = 100u;
  script_tap_at(2u, 120, 225);  /* Album, selecting first unseen card. */
  script_tap_at(4u, 35, 45);    /* Fruit. */
  script_tap_at(22u, 90, 45);   /* Weapons. */
  script_tap_at(42u, 150, 45);  /* Bombs. */
  script_tap_at(62u, 205, 45);  /* Skills. */
  script_tap_at(82u, 120, 225); /* Back to title. */
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
  CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(), "cards_v1",
                     &data, &size) == H2_PAL_OK);
  h2_fruit_slice_cards_t revisited;
  h2_fruit_slice_cards_init(&revisited, 240, 240, 1u);
  CHECK(h2_fruit_slice_cards_decode(&revisited, data, size));
  CHECK(memcmp(collection.counts, revisited.counts,
               sizeof(collection.counts)) == 0);
  CHECK(revisited.unseen != collection.unseen);
  h2_pal_mem_free(h2_desktop_platform_default_allocator(), data);
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
  /* Unknown and malformed collections stay byte-identical even after a new
   * run naturally earns cards. Coin/audio keys remain independent. */
  for (unsigned invalid = 0; invalid < 2u; ++invalid) {
    uint8_t bytes[H2_FRUIT_SLICE_CARDS_BYTES];
    h2_fruit_slice_cards_encode(&collection, bytes);
    bytes[invalid == 0u ? 4u : 16u] = invalid == 0u ? 2u : 100u;
    CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                           H2_FRUIT_SLICE_PREF_NAMESPACE,
                           H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
    CHECK(ns->set_blob(ns, "cards_v1", bytes, sizeof(bytes)) == H2_PAL_OK);
    CHECK(ns->commit(ns) == H2_PAL_OK);
    CHECK(ns->close(ns) == H2_PAL_OK);
    reset(400u);
    fx.pref_override = h2_sqlite_pref_api(storage);
    fx.fast_clock = true;
    fx.autoplay_until = 400u;
    CHECK(run() == H2_PAL_OK);
    CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                           H2_FRUIT_SLICE_PREF_NAMESPACE,
                           H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
    CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(), "cards_v1",
                       &data, &size) == H2_PAL_OK);
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

static void test_audio_backpressure_keeps_stream_playing(void) {
  reset(40u);
  fx.use_audio = true;
  fx.audio_block_writes = 3u;
  fx.touch_from_frame = 5u;
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 60, 288};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 400, 288};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 400, 288};
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.audio_block_writes == 0u && fx.audio_successful_writes > 0u);
  CHECK(fx.audio_writes == fx.audio_successful_writes + 3u);
  CHECK(fx.audio_opens == 1u && fx.audio_closes == 1u);
  CHECK(fx.audio_starts == 1u && fx.audio_stops == 1u);
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

/* Wrap only the real provider boundary; all game/input/save paths stay live. */
static struct {
  const h2_pal_pref_api_t* real;
  h2_pal_pref_namespace_get_blob_fn get_blob;
  h2_pal_pref_namespace_set_blob_fn set_blob;
  h2_pal_pref_namespace_commit_fn commit;
  int first_read_error;
  unsigned reads, successful_reads, recover_after_frame;
  unsigned writes, commits, write_failures, commit_failures;
  bool writing_cards;
} card_fault;

static int recovering_get(h2_pal_pref_namespace_t* ns,
                          const h2_pal_mem_api_t* mem, const char* key,
                          void** data, size_t* size) {
  if (strcmp(key, "cards_v1") == 0) {
    ++card_fault.reads;
    if (card_fault.first_read_error != H2_PAL_OK) {
      const int rc = card_fault.first_read_error;
      card_fault.first_read_error = H2_PAL_OK;
      return rc;
    }
    if (fx.frames < card_fault.recover_after_frame)
      return H2_PAL_ERR_WOULD_BLOCK;
    ++card_fault.successful_reads;
  }
  return card_fault.get_blob(ns, mem, key, data, size);
}

static int recovering_set(h2_pal_pref_namespace_t* ns, const char* key,
                          const void* data, size_t size) {
  card_fault.writing_cards = strcmp(key, "cards_v1") == 0;
  if (card_fault.writing_cards) {
    ++card_fault.writes;
    if (card_fault.write_failures) {
      --card_fault.write_failures;
      return H2_PAL_ERR_IO;
    }
  }
  return card_fault.set_blob(ns, key, data, size);
}

static int recovering_commit(h2_pal_pref_namespace_t* ns) {
  if (card_fault.writing_cards) {
    ++card_fault.commits;
    if (card_fault.commit_failures) {
      --card_fault.commit_failures;
      return H2_PAL_ERR_IO;
    }
  }
  return card_fault.commit(ns);
}

static int recovering_open(void* user, const char* name,
                           h2_pal_pref_open_mode_t mode,
                           h2_pal_pref_namespace_t** out) {
  (void)user;
  const int rc = h2_pal_pref_open(card_fault.real, name, mode, out);
  if (rc == H2_PAL_OK) {
    card_fault.get_blob = (*out)->get_blob;
    card_fault.set_blob = (*out)->set_blob;
    card_fault.commit = (*out)->commit;
    card_fault.writing_cards = false;
    (*out)->get_blob = recovering_get;
    (*out)->set_blob = recovering_set;
    (*out)->commit = recovering_commit;
  }
  return rc;
}
static const h2_pal_pref_vtable_t recovering_vtable = {.open = recovering_open};
static const h2_pal_pref_api_t recovering_api = {.vtable = &recovering_vtable};

static void read_sqlite_cards(h2_sqlite_t* storage,
                              h2_fruit_slice_cards_t* cards) {
  h2_pal_pref_namespace_t* ns = NULL;
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
  void* data = NULL;
  size_t size = 0u;
  CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(), "cards_v1",
                     &data, &size) == H2_PAL_OK);
  h2_fruit_slice_cards_init(cards, 480, 480, 1u);
  CHECK(h2_fruit_slice_cards_decode(cards, data, size));
  h2_pal_mem_free(h2_desktop_platform_default_allocator(), data);
  CHECK(ns->close(ns) == H2_PAL_OK);
}

static void test_cards_transient_sqlite_read(void) {
  const char* temporary = getenv("TEST_TMPDIR");
  CHECK(temporary != NULL);
  for (unsigned failure = 0; failure < 3u; ++failure) {
    char database[512];
    (void)snprintf(database, sizeof(database), "%s/cards-recovery-%u.sqlite",
                   temporary, failure);
    const h2_sqlite_config_t config = {.path = database};
    h2_sqlite_t* storage = NULL;
    CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
    h2_fruit_slice_cards_t initial;
    h2_fruit_slice_cards_init(&initial, 480, 480, 1u);
    initial.counts[H2_FRUIT_SLICE_CARD_WATERMELON] = 5u;
    initial.counts[H2_FRUIT_SLICE_CARD_ORANGE] = 99u;
    initial.counts[H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE] = 5u;
    initial.unseen = 1u << H2_FRUIT_SLICE_CARD_ORANGE;
    uint8_t bytes[H2_FRUIT_SLICE_CARDS_BYTES];
    h2_fruit_slice_cards_encode(&initial, bytes);
    h2_pal_pref_namespace_t* ns = NULL;
    CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                           H2_FRUIT_SLICE_PREF_NAMESPACE,
                           H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
    CHECK(ns->set_blob(ns, "cards_v1", bytes, sizeof(bytes)) == H2_PAL_OK);
    CHECK(ns->commit(ns) == H2_PAL_OK && ns->close(ns) == H2_PAL_OK);
    memset(&card_fault, 0, sizeof(card_fault));
    card_fault.real = h2_sqlite_pref_api(storage);
    card_fault.first_read_error =
        failure == 0u ? H2_PAL_ERR_IO : H2_PAL_ERR_WOULD_BLOCK;
    card_fault.write_failures = failure != 1u ? 1u : 0u;
    card_fault.commit_failures = failure != 0u ? 1u : 0u;
    /* The third case recovers after real play has already earned duplicates
     * and a new bomb, exercising pending increments in the public App. */
    card_fault.recover_after_frame = failure == 2u ? 330u : 0u;
    reset(400u);
    fx.pref_override = &recovering_api;
    fx.fast_clock = true;
    fx.autoplay_until = 400u;
    CHECK(run() == H2_PAL_OK);
    CHECK(card_fault.successful_reads == 1u);
    CHECK(failure == 2u ? card_fault.reads > 2u : card_fault.reads == 2u);
    /* Successful merge is never repeated by writes. */
    CHECK(card_fault.writes >= 2u && card_fault.write_failures == 0u &&
          card_fault.commit_failures == 0u);
    h2_sqlite_destroy(storage);
    CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
    h2_fruit_slice_cards_t saved;
    read_sqlite_cards(storage, &saved);
    CHECK(saved.counts[H2_FRUIT_SLICE_CARD_WATERMELON] >= 5u);
    CHECK(saved.counts[H2_FRUIT_SLICE_CARD_ORANGE] == 99u);
    CHECK(saved.counts[H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE] == 6u);
    CHECK(saved.counts[H2_FRUIT_SLICE_CARD_BOMB] == 1u);
    CHECK((saved.unseen & (1u << H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE)) == 0u);
    CHECK((saved.unseen & (1u << H2_FRUIT_SLICE_CARD_BOMB)) != 0u);
    CHECK((saved.unseen & initial.unseen) == initial.unseen);
    reset(20u);
    fx.pref_override = h2_sqlite_pref_api(storage);
    fx.fast_clock = true;
    fx.touch_from_frame = 2u;
    script_tap(240,
               450); /* Load the saved album and inspect the old NEW orange. */
    CHECK(run() == H2_PAL_OK);
    h2_fruit_slice_cards_t reopened;
    read_sqlite_cards(storage, &reopened);
    CHECK(memcmp(saved.counts, reopened.counts, sizeof(saved.counts)) == 0);
    CHECK(reopened.unseen == (saved.unseen & ~initial.unseen));
    h2_sqlite_destroy(storage);
    CHECK(remove(database) == 0);
  }
}

static void test_cards_write_retry_and_read_error(void) {
  memset(&pref, 0, sizeof(pref));
  pref.cards_enabled = true;
  pref.cards_failures = 1u;
  reset(400u);
  fx.use_pref = true;
  fx.fast_clock = true;
  fx.autoplay_until = 400u;
  CHECK(run() == H2_PAL_OK);
  CHECK(pref.cards_commits >= 2u && pref.cards_failures == 0u);
  h2_fruit_slice_cards_t collection;
  h2_fruit_slice_cards_init(&collection, 480, 480, 1u);
  CHECK(h2_fruit_slice_cards_decode(&collection, pref.cards_saved,
                                    sizeof(pref.cards_saved)));
  CHECK(h2_fruit_slice_cards_owned_total(&collection) > 0u);
  uint8_t saved[H2_FRUIT_SLICE_CARDS_BYTES];
  memcpy(saved, pref.cards_saved, sizeof(saved));
  const unsigned commits = pref.cards_commits;
  pref.cards_read_result = H2_PAL_ERR_IO;
  reset(400u);
  fx.use_pref = true;
  fx.fast_clock = true;
  fx.autoplay_until = 400u;
  CHECK(run() == H2_PAL_OK);
  CHECK(pref.cards_commits == commits);
  CHECK(memcmp(saved, pref.cards_saved, sizeof(saved)) == 0);
  const unsigned reads = pref.cards_reads;
  reset(60u);
  fx.display.info.width = fx.display.info.height = 240;
  fx.touch.info.width = fx.touch.info.height = 240;
  fx.use_pref = true;
  fx.fast_clock = true;
  fx.touch_from_frame = 2u;
  fx.capture_name = "actual-app-unsaved-retry-240";
  script_tap(120, 225);
  CHECK(run() == H2_PAL_OK);
  CHECK(pref.cards_reads - reads >= 2u && pref.cards_reads - reads <= 6u);
  CHECK(pref.cards_commits == commits);
  CHECK(memcmp(saved, pref.cards_saved, sizeof(saved)) == 0);
}

int main(void) {
  test_cards_transient_sqlite_read();
  test_cards_write_retry_and_read_error();
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
  test_audio_backpressure_keeps_stream_playing();
  test_audio_close_errors_release_runtime_wrapped_track();
  puts("fruit slice app tests passed");
  return 0;
}
