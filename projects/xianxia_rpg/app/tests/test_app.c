/* Run loop tests through the public entry with fake Display/Touch providers
 * and a Desktop-backed Runtime. */
#include "h2_desktop_platform.h"
#include "h2_pal_unsupported.h"
#include "h2_xianxia_rpg.h"
#include "h2_xianxia_rpg_game.h"

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
#define PREF_MAX 16
#define PREF_VALUE_MAX 256

/* Each normal cc_test selects its group while sharing this fixture and
 * the production App driver. Keep the default target on chapters 1–7. */
#ifndef H2_XIANXIA_RPG_TEST_EIGHTH_RECORD_OWNERSHIP
#define H2_XIANXIA_RPG_TEST_EIGHTH_RECORD_OWNERSHIP 0
#endif
#ifndef H2_XIANXIA_RPG_TEST_NINTH_RECORD_OWNERSHIP
#define H2_XIANXIA_RPG_TEST_NINTH_RECORD_OWNERSHIP 0
#endif

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

/* In-memory Preferences, enough for the App's u32 and blob records. */
typedef struct pref_entry {
  bool used;
  char key[32];
  h2_pal_pref_entry_type_t type;
  uint8_t value[PREF_VALUE_MAX];
  size_t size;
  unsigned writes;
} pref_entry_t;

typedef struct fake_pref {
  pref_entry_t entries[PREF_MAX];
  h2_pal_pref_namespace_t ns;
  unsigned opens, commits, write_attempts;
  const char* read_error_key;
  int read_error, write_error, commit_error, open_error;
} fake_pref_t;

static fake_pref_t prefs;

static pref_entry_t* pref_find(const char* key) {
  for (size_t i = 0; i < PREF_MAX; ++i)
    if (prefs.entries[i].used && strcmp(prefs.entries[i].key, key) == 0)
      return &prefs.entries[i];
  return NULL;
}

static pref_entry_t* pref_put(const char* key, h2_pal_pref_entry_type_t type,
                              const void* data, size_t size) {
  pref_entry_t* e = pref_find(key);
  for (size_t i = 0; e == NULL && i < PREF_MAX; ++i)
    if (!prefs.entries[i].used) e = &prefs.entries[i];
  CHECK(e != NULL && size <= PREF_VALUE_MAX && strlen(key) < sizeof(e->key));
  e->used = true;
  (void)snprintf(e->key, sizeof(e->key), "%s", key);
  e->type = type;
  memcpy(e->value, data, size);
  e->size = size;
  ++e->writes;
  return e;
}

static int ns_close(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  return H2_PAL_OK;
}
static int ns_get_u32(h2_pal_pref_namespace_t* ns, const char* key,
                      uint32_t* out) {
  (void)ns;
  const pref_entry_t* e = pref_find(key);
  if (e == NULL) return H2_PAL_ERR_NOT_FOUND;
  if (e->type != H2_PAL_PREF_ENTRY_U32) return H2_PAL_ERR_FORMAT;
  memcpy(out, e->value, sizeof(*out));
  return H2_PAL_OK;
}
static int ns_set_u32(h2_pal_pref_namespace_t* ns, const char* key,
                      uint32_t value) {
  (void)ns;
  (void)pref_put(key, H2_PAL_PREF_ENTRY_U32, &value, sizeof(value));
  return H2_PAL_OK;
}
static int ns_get_blob(h2_pal_pref_namespace_t* ns,
                       const h2_pal_mem_api_t* allocator, const char* key,
                       void** out, size_t* out_len) {
  (void)ns;
  if (prefs.read_error_key != NULL && strcmp(key, prefs.read_error_key) == 0)
    return prefs.read_error;
  const pref_entry_t* e = pref_find(key);
  if (e == NULL) return H2_PAL_ERR_NOT_FOUND;
  if (e->type != H2_PAL_PREF_ENTRY_BLOB) return H2_PAL_ERR_FORMAT;
  void* data = h2_pal_mem_alloc(allocator, e->size == 0u ? 1u : e->size);
  if (data == NULL) return H2_PAL_ERR_NO_MEMORY;
  memcpy(data, e->value, e->size);
  *out = data;
  *out_len = e->size;
  return H2_PAL_OK;
}
static int ns_set_blob(h2_pal_pref_namespace_t* ns, const char* key,
                       const void* data, size_t size) {
  (void)ns;
  ++prefs.write_attempts;
  if (prefs.write_error != H2_PAL_OK) return prefs.write_error;
  (void)pref_put(key, H2_PAL_PREF_ENTRY_BLOB, data, size);
  return H2_PAL_OK;
}
static int ns_commit(h2_pal_pref_namespace_t* ns) {
  (void)ns;
  ++prefs.commits;
  return prefs.commit_error;
}
static int pref_open(void* user, const char* name_space,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)user;
  (void)mode;
  CHECK(strcmp(name_space, H2_XIANXIA_RPG_PREF_NAMESPACE) == 0);
  ++prefs.opens;
  if (prefs.open_error != H2_PAL_OK) return prefs.open_error;
  prefs.ns = (h2_pal_pref_namespace_t){.close = ns_close,
                                       .get_blob = ns_get_blob,
                                       .set_blob = ns_set_blob,
                                       .get_u32 = ns_get_u32,
                                       .set_u32 = ns_set_u32,
                                       .commit = ns_commit};
  *out = &prefs.ns;
  return H2_PAL_OK;
}
static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};
static const h2_pal_pref_api_t pref_api = {.vtable = &pref_vtable};
static bool use_prefs;

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
  config.pref = use_prefs ? &pref_api : h2_pal_unsupported_pref_api();
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

/* The v0.1 words of a finished chapter 1, as the released build stored
 * them: Lv4, two pills, 6 battles and 39 turns. */
static void plant_v01_clear(void) {
  const h2_xianxia_rpg_save_t clear = {.stage = H2_XIANXIA_RPG_STAGE_CLEARED,
                                       .level = 4u,
                                       .exp = 99u,
                                       .pills = 2u,
                                       .flags = H2_XIANXIA_RPG_FLAG_PILLS_GIVEN,
                                       .map = H2_XIANXIA_RPG_MAP_VILLAGE,
                                       .x = 4u,
                                       .y = 4u,
                                       .battles = 6u,
                                       .turns = 39u};
  uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
  h2_xianxia_rpg_save_pack(&clear, words);
  static const char* const keys[] = {
      H2_XIANXIA_RPG_PREF_SAVE0_KEY, H2_XIANXIA_RPG_PREF_SAVE1_KEY,
      H2_XIANXIA_RPG_PREF_SAVE2_KEY, H2_XIANXIA_RPG_PREF_SAVE3_KEY};
  for (size_t i = 0; i < 4u; ++i)
    pref_put(keys[i], H2_PAL_PREF_ENTRY_U32, &words[i], sizeof(words[i]))
        ->writes = 0;
}

static uint32_t v01_tag(void) {
  uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
  static const char* const keys[] = {
      H2_XIANXIA_RPG_PREF_SAVE0_KEY, H2_XIANXIA_RPG_PREF_SAVE1_KEY,
      H2_XIANXIA_RPG_PREF_SAVE2_KEY, H2_XIANXIA_RPG_PREF_SAVE3_KEY};
  for (size_t i = 0; i < 4u; ++i) {
    const pref_entry_t* e = pref_find(keys[i]);
    CHECK(e != NULL && e->writes == 0u);
    memcpy(&words[i], e->value, sizeof(words[i]));
  }
  return h2_xianxia_rpg_chapter1_tag(words, 0xFu);
}

/* Upgrading from v0.1: the clear is copied into the chapter record at
 * start, and choosing chapter 2 saves a journey stamped as newer than the
 * untouched save0..save3 words. */
static void test_upgrade_keeps_v01_keys(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_v01_clear();
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 30u;
  CHECK(run_once() == H2_PAL_OK);
  const pref_entry_t* chapters = pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
  CHECK(chapters != NULL && chapters->writes == 1u);
  h2_xianxia_rpg_progress_t progress;
  CHECK(h2_xianxia_rpg_progress_decode(chapters->value, chapters->size,
                                       &progress));
  CHECK(progress.cleared == 1u && progress.chapters[0].level == 4u);
  CHECK(progress.chapters[0].battles == 6u &&
        progress.chapters[0].turns == 39u);
  CHECK(pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY) == NULL);
  const uint32_t tag = v01_tag();

  /* A second start writes nothing; then chapters -> chapter 2 -> card. */
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 200u;
  h2_xianxia_rpg_game_t layout;
  h2_xianxia_rpg_game_init(&layout, WIDTH, HEIGHT, 1u, NULL);
  const h2_xianxia_rpg_rect_t* chapters_button =
      &layout.layout.title_buttons[2];
  const h2_xianxia_rpg_rect_t* row = &layout.layout.chapter_rows[1];
  script_tap(40u, chapters_button->x + chapters_button->w / 2,
             chapters_button->y + chapters_button->h / 2);
  script_tap(80u, row->x + row->w / 2, row->y + row->h / 2);
  script_tap(190u, WIDTH / 2, HEIGHT / 2);
  CHECK(run_once() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  CHECK(pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY)->writes == 1u);
  const pref_entry_t* journey = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY);
  CHECK(journey != NULL && journey->writes == 1u);
  h2_xianxia_rpg_save_t save;
  uint32_t journey_tag = 0;
  CHECK(h2_xianxia_rpg_journey_decode(journey->value, journey->size, &save,
                                      &journey_tag));
  CHECK(save.chapter == 2u && save.stage == H2_XIANXIA_RPG_STAGE_C2_ARRIVE);
  CHECK(save.level == 4u && save.pills == 2u);
  CHECK(save.flags & H2_XIANXIA_RPG_FLAG_C1_CLEARED);
  CHECK(journey_tag == tag);
  (void)v01_tag();
  use_prefs = false;
}

/* A corrupt journey is ignored and left as it was. */
static void test_corrupt_journey_is_kept(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  uint8_t junk[H2_XIANXIA_RPG_JOURNEY_BYTES];
  memset(junk, 0x5A, sizeof(junk));
  pref_put(H2_XIANXIA_RPG_PREF_JOURNEY_KEY, H2_PAL_PREF_ENTRY_BLOB, junk,
           sizeof(junk))
      ->writes = 0;
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 30u;
  CHECK(run_once() == H2_PAL_OK);
  const pref_entry_t* journey = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY);
  CHECK(journey->writes == 0u &&
        memcmp(journey->value, junk, sizeof(junk)) == 0);
  CHECK(pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY) == NULL);
  CHECK(pref_find(H2_XIANXIA_RPG_PREF_SAVE0_KEY) == NULL);
  use_prefs = false;
}

/* Combine a valid legacy clear with independently missing, valid, corrupt,
 * wrong-type and IO-failed blob reads. Only unreadable chapters block the
 * automatic chapter write; every journey and the legacy words stay intact. */
static void test_import_record_combinations(void) {
  for (unsigned chapter_state = 0; chapter_state < 5u; ++chapter_state) {
    for (unsigned journey_state = 0; journey_state < 5u; ++journey_state) {
      memset(&prefs, 0, sizeof(prefs));
      use_prefs = true;
      plant_v01_clear();
      const uint32_t tag = v01_tag();
      const h2_xianxia_rpg_progress_t progress = {
          .cleared = 2u, .chapters = {[1] = {.level = 6u, .pills = 3u}}};
      uint8_t chapters[H2_XIANXIA_RPG_PROGRESS_BYTES];
      h2_xianxia_rpg_progress_encode(&progress, chapters);
      const h2_xianxia_rpg_save_t save = {
          .chapter = 2u,
          .stage = H2_XIANXIA_RPG_STAGE_C2_ARRIVE,
          .level = 5u,
          .pills = 3u,
          .map = H2_XIANXIA_RPG_MAP_DOCK,
          .x = 8u,
          .y = 5u,
          .rest_map = H2_XIANXIA_RPG_MAP_DOCK,
          .rest_x = 8u,
          .rest_y = 5u};
      uint8_t journey[H2_XIANXIA_RPG_JOURNEY_BYTES];
      h2_xianxia_rpg_journey_encode(&save, tag, journey);
      const char* keys[] = {H2_XIANXIA_RPG_PREF_CHAPTERS_KEY,
                            H2_XIANXIA_RPG_PREF_JOURNEY_KEY};
      const unsigned states[] = {chapter_state, journey_state};
      uint8_t* values[] = {chapters, journey};
      const size_t sizes[] = {sizeof(chapters), sizeof(journey)};
      for (unsigned i = 0; i < 2u; ++i) {
        if (states[i] == 0u) continue;
        if (states[i] >= 2u) memset(values[i], 0x5A, sizes[i]);
        pref_put(
            keys[i],
            states[i] == 3u ? H2_PAL_PREF_ENTRY_U32 : H2_PAL_PREF_ENTRY_BLOB,
            values[i], sizes[i])
            ->writes = 0u;
      }
      /* Exercise IO failures independently (both failing is equivalent to
       * chapters failing for the migration decision). */
      if (chapter_state == 4u || journey_state == 4u) {
        prefs.read_error_key = keys[chapter_state == 4u ? 0 : 1];
        prefs.read_error = H2_PAL_ERR_IO;
      }
      reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
      fx.exit_after = 3u;
      CHECK(run_once() == H2_PAL_OK);
      CHECK(v01_tag() == tag);
      const pref_entry_t* c = pref_find(keys[0]);
      CHECK(c != NULL);
      if (chapter_state >= 2u) {
        CHECK(c->writes == 0u && c->size == sizeof(chapters));
        CHECK(memcmp(c->value, chapters, sizeof(chapters)) == 0);
        CHECK(prefs.write_attempts == 0u && prefs.commits == 0u);
      } else {
        h2_xianxia_rpg_progress_t imported;
        CHECK(c->writes == 1u);
        CHECK(h2_xianxia_rpg_progress_decode(c->value, c->size, &imported));
        CHECK(imported.cleared == (chapter_state == 1u ? 3u : 1u));
        CHECK(imported.chapters[0].battles == 6u &&
              imported.chapters[0].turns == 39u);
        if (chapter_state == 1u) CHECK(imported.chapters[1].level == 6u);
      }
      const pref_entry_t* j = pref_find(keys[1]);
      if (journey_state == 0u)
        CHECK(j == NULL);
      else {
        CHECK(j != NULL && j->writes == 0u && j->size == sizeof(journey));
        CHECK(memcmp(j->value, journey, sizeof(journey)) == 0);
      }
    }
  }
  use_prefs = false;
}

static void test_three_key_import_and_write_failures(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_v01_clear();
  pref_find(H2_XIANXIA_RPG_PREF_SAVE3_KEY)->used = false;
  prefs.write_error = H2_PAL_ERR_IO;
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 3u;
  CHECK(run_once() == H2_PAL_OK);
  CHECK(prefs.write_attempts == 1u);
  CHECK(pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY) == NULL);
  CHECK(pref_find(H2_XIANXIA_RPG_PREF_SAVE3_KEY) == NULL);
  prefs.write_error = H2_PAL_OK;
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 3u;
  CHECK(run_once() == H2_PAL_OK);
  const pref_entry_t* chapters = pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
  h2_xianxia_rpg_progress_t progress;
  CHECK(chapters != NULL && chapters->writes == 1u);
  CHECK(h2_xianxia_rpg_progress_decode(chapters->value, chapters->size,
                                       &progress));
  CHECK(progress.cleared == 1u && progress.chapters[0].level == 4u);
  CHECK(progress.chapters[0].battles == 0u && progress.chapters[0].turns == 0u);
  CHECK(pref_find(H2_XIANXIA_RPG_PREF_SAVE3_KEY) == NULL);
  for (size_t i = 0; i < PREF_MAX; ++i)
    if (prefs.entries[i].used && prefs.entries[i].type == H2_PAL_PREF_ENTRY_U32)
      CHECK(prefs.entries[i].writes == 0u);
  /* Optional persistence remains nonfatal on open and commit errors. */
  prefs.open_error = H2_PAL_ERR_IO;
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 3u;
  CHECK(run_once() == H2_PAL_OK);
  CHECK(chapters->writes == 1u);
  prefs.open_error = H2_PAL_OK;
  pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY)->writes = 0u;
  pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY)->used = false;
  prefs.commit_error = H2_PAL_ERR_IO;
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 3u;
  CHECK(run_once() == H2_PAL_OK);
  CHECK(pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY)->writes == 1u);
  use_prefs = false;
}

/* Released v0.2 bytes (Lv7, chapter-2 ending, four wins/23 turns), not
 * encoded by the candidate under test. An actual browser upgrade also covers
 * freshly played release records. */
static void plant_released_v02(void) {
  static const uint8_t bytes[32] = {
      0x02, 0x02, 0x0b, 0x07, 0x08, 0x03, 0x06, 0x04, 0x03, 0x0d, 0x03,
      0x01, 0x3e, 0x01, 0x00, 0x00, 0x00, 0xff, 0x1f, 0x00, 0x04, 0x00,
      0x17, 0x00, 0x8d, 0x5e, 0x7c, 0x27, 0xe7, 0xca, 0x2b, 0x8b};
  pref_put(H2_XIANXIA_RPG_PREF_JOURNEY_KEY, H2_PAL_PREF_ENTRY_BLOB, bytes,
           sizeof(bytes))
      ->writes = 0u;
}

static void test_third_chapter_keeps_released_journey(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_released_v02();
  const pref_entry_t old = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY);
  h2_xianxia_rpg_save_t released;
  CHECK(h2_xianxia_rpg_journey_decode(old.value, old.size, &released, NULL));
  CHECK(released.chapter == 2u && released.level == 7u &&
        released.turns == 23u);
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 210u;
  h2_xianxia_rpg_game_t layout;
  h2_xianxia_rpg_game_init(&layout, WIDTH, HEIGHT, 1u, NULL);
  const h2_xianxia_rpg_rect_t* b = &layout.layout.title_buttons[2];
  const h2_xianxia_rpg_rect_t* row = &layout.layout.chapter_rows[2];
  script_tap(40u, b->x + b->w / 2, b->y + b->h / 2);
  script_tap(80u, row->x + row->w / 2, row->y + row->h / 2);
  script_tap(190u, WIDTH / 2, HEIGHT / 2);
  CHECK(run_once() == H2_PAL_OK);
  CHECK(memcmp(&old, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY), sizeof(old)) ==
        0);
  const pref_entry_t* j3 = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY3_KEY);
  h2_xianxia_rpg_save_t save;
  uint32_t tag = 0;
  CHECK(j3 != NULL && j3->writes == 1u);
  CHECK(h2_xianxia_rpg_journey_decode(j3->value, j3->size, &save, &tag));
  CHECK(save.chapter == 3u && save.map == H2_XIANXIA_RPG_MAP_RAIN_PATH);
  CHECK(tag == h2_xianxia_rpg_predecessor_tag(0u, old.value, old.size));
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 3u;
  CHECK(run_once() == H2_PAL_OK && j3->writes == 1u);
  CHECK(memcmp(&old, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY), sizeof(old)) ==
        0);
  /* Unknown future chapter, corrupt bytes, wrong type and a failed read
   * never replace journey3 on startup, even with a migrating legacy clear. */
  for (unsigned state = 0; state < 4u; ++state) {
    memset(&prefs, 0, sizeof(prefs));
    plant_v01_clear();
    plant_released_v02();
    uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
    save.chapter = 4u;
    h2_xianxia_rpg_journey_encode(&save, 0u, bytes);
    if (state == 1u) bytes[9] ^= 0x40u;
    pref_entry_t* e =
        pref_put(H2_XIANXIA_RPG_PREF_JOURNEY3_KEY,
                 state == 2u ? H2_PAL_PREF_ENTRY_U32 : H2_PAL_PREF_ENTRY_BLOB,
                 bytes, sizeof(bytes));
    e->writes = 0u;
    if (state == 3u) {
      prefs.read_error_key = H2_XIANXIA_RPG_PREF_JOURNEY3_KEY;
      prefs.read_error = H2_PAL_ERR_IO;
    }
    reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
    fx.exit_after = 3u;
    CHECK(run_once() == H2_PAL_OK);
    CHECK(e->writes == 0u && memcmp(e->value, bytes, sizeof(bytes)) == 0);
    CHECK(pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY)->writes == 0u);
    (void)v01_tag();
  }
  use_prefs = false;
}

/* Captured from the actual published v0.3 WASM after a fresh mouse-driven
 * third-chapter clear: Lv7, three pills, one win, seventeen turns. */
static void plant_released_v03(void) {
  static const uint8_t journey[32] = {
      0x02, 0x03, 0x10, 0x07, 0x03, 0x08, 0x04, 0x02, 0x08, 0x03, 0x07,
      0x00, 0x4f, 0x01, 0x00, 0x00, 0xf1, 0x00, 0xe0, 0x03, 0x01, 0x00,
      0x11, 0x00, 0x9c, 0xd3, 0x4e, 0xcb, 0x1b, 0xeb, 0xa0, 0xb4};
  static const uint8_t chapters[168] = {
      [0] = 2,      [1] = 10,     [2] = 4,      [36] = 7,
      [37] = 3,     [40] = 0x4f,  [41] = 1,     [44] = 0xf1,
      [46] = 0xe0,  [47] = 3,     [48] = 1,     [50] = 0x11,
      [164] = 0x98, [165] = 0xfb, [166] = 0xaa, [167] = 0x5e};
  pref_put(H2_XIANXIA_RPG_PREF_JOURNEY3_KEY, H2_PAL_PREF_ENTRY_BLOB, journey,
           sizeof(journey))
      ->writes = 0;
  pref_put(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY, H2_PAL_PREF_ENTRY_BLOB, chapters,
           sizeof(chapters))
      ->writes = 0;
}
static void test_fourth_chapter_keeps_released_records(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_released_v02();
  plant_released_v03();
  const pref_entry_t j2 = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY);
  const pref_entry_t j3 = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY3_KEY);
  const pref_entry_t chapters = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
  h2_xianxia_rpg_save_t save;
  h2_xianxia_rpg_progress_t progress;
  CHECK(h2_xianxia_rpg_journey_decode(j3.value, j3.size, &save, NULL));
  CHECK(save.chapter == 3u && save.turns == 17u && save.level == 7u);
  CHECK(
      h2_xianxia_rpg_progress_decode(chapters.value, chapters.size, &progress));
  CHECK(progress.cleared == 4u && progress.chapters[2].turns == 17u);
  h2_xianxia_rpg_game_t layout;
  h2_xianxia_rpg_game_init(&layout, WIDTH, HEIGHT, 1, NULL);
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 210u;
  const h2_xianxia_rpg_rect_t* b = &layout.layout.title_buttons[2];
  const h2_xianxia_rpg_rect_t* row = &layout.layout.chapter_rows[3];
  script_tap(40u, b->x + b->w / 2, b->y + b->h / 2);
  script_tap(80u, row->x + row->w / 2, row->y + row->h / 2);
  script_tap(190u, WIDTH / 2, HEIGHT / 2);
  CHECK(run_once() == H2_PAL_OK);
  CHECK(memcmp(&j2, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY), sizeof(j2)) ==
        0);
  CHECK(memcmp(&j3, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY3_KEY), sizeof(j3)) ==
        0);
  CHECK(memcmp(&chapters, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
               sizeof(chapters)) == 0);
  const pref_entry_t* j4 = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY4_KEY);
  uint32_t tag = 0;
  CHECK(j4 && j4->writes == 1u &&
        h2_xianxia_rpg_journey_decode(j4->value, j4->size, &save, &tag));
  CHECK(save.chapter == 4u && save.level == 7u && save.exp == 335u &&
        save.pills == 3u);
  CHECK(tag == (h2_xianxia_rpg_predecessor_tag(0u, j2.value, j2.size) ^
                h2_xianxia_rpg_predecessor_tag(0u, j3.value, j3.size)));
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 3u;
  CHECK(run_once() == H2_PAL_OK && j4->writes == 1u);
  for (unsigned state = 0; state < 5u; ++state) {
    memset(&prefs, 0, sizeof(prefs));
    plant_v01_clear();
    plant_released_v02();
    plant_released_v03();
    uint8_t bytes[32];
    save.chapter = state == 0u ? 5u : 4u;
    h2_xianxia_rpg_journey_encode(&save, 0, bytes);
    if (state == 1u) bytes[10] ^= 0x40u;
    if (state == 4u) {
      bytes[0] = 3u;
      uint32_t hash = 2166136261u ^ 0x4A524E59u;
      for (size_t i = 0; i < 28u; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
      }
      for (unsigned i = 0; i < 4u; ++i)
        bytes[28u + i] = (uint8_t)(hash >> (8u * i));
    }
    pref_entry_t* e =
        pref_put(H2_XIANXIA_RPG_PREF_JOURNEY4_KEY,
                 state == 2u ? H2_PAL_PREF_ENTRY_U32 : H2_PAL_PREF_ENTRY_BLOB,
                 bytes, sizeof(bytes));
    e->writes = 0;
    if (state == 3u) {
      prefs.read_error_key = H2_XIANXIA_RPG_PREF_JOURNEY4_KEY;
      prefs.read_error = H2_PAL_ERR_IO;
    }
    reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
    fx.exit_after = 3u;
    CHECK(run_once() == H2_PAL_OK && e->writes == 0u &&
          memcmp(e->value, bytes, sizeof(bytes)) == 0);
    CHECK(pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY)->writes == 0u &&
          pref_find(H2_XIANXIA_RPG_PREF_JOURNEY3_KEY)->writes == 0u);
  }
  use_prefs = false;
}

/* Actual published v0.4 WASM, newly played with mouse input: chapter 4
 * cleared at Lv8, three pills, one win and nine turns. No synthetic save. */
static void plant_released_v04(void) {
  static const uint8_t journey[32] = {
      [0] = 2,     [1] = 4,     [2] = 0x15,  [3] = 8,  [4] = 3,  [5] = 9,
      [6] = 5,     [7] = 3,     [8] = 9,     [9] = 7,  [10] = 5, [12] = 0xd1,
      [13] = 1,    [16] = 0x1f, [19] = 0x3c, [20] = 1, [22] = 9, [28] = 0x8b,
      [29] = 0x73, [30] = 0xc8, [31] = 0x6e};
  static const uint8_t chapters[168] = {
      [0] = 2,     [1] = 10,     [2] = 8,      [52] = 8,     [53] = 3,
      [56] = 0xd1, [57] = 1,     [60] = 0x1f,  [63] = 0x3c,  [64] = 1,
      [66] = 9,    [164] = 0x8e, [165] = 0x16, [166] = 0xb5, [167] = 0x2b};
  pref_put(H2_XIANXIA_RPG_PREF_JOURNEY4_KEY, H2_PAL_PREF_ENTRY_BLOB, journey,
           sizeof(journey))
      ->writes = 0;
  pref_put(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY, H2_PAL_PREF_ENTRY_BLOB, chapters,
           sizeof(chapters))
      ->writes = 0;
}
static void test_fifth_chapter_keeps_released_records(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_released_v02();
  plant_released_v03();
  plant_released_v04();
  const pref_entry_t j2 = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY),
                     j3 = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY3_KEY),
                     j4 = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY4_KEY),
                     chapters = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
  h2_xianxia_rpg_game_t layout;
  h2_xianxia_rpg_game_init(&layout, WIDTH, HEIGHT, 1, NULL);
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 210;
  const h2_xianxia_rpg_rect_t* b = &layout.layout.title_buttons[2];
  const h2_xianxia_rpg_rect_t* row = &layout.layout.chapter_rows[4];
  script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
  script_tap(80, row->x + row->w / 2, row->y + row->h / 2);
  script_tap(190, WIDTH / 2, HEIGHT / 2);
  CHECK(run_once() == H2_PAL_OK);
  CHECK(memcmp(&j2, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY_KEY), sizeof(j2)) ==
        0);
  CHECK(memcmp(&j3, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY3_KEY), sizeof(j3)) ==
        0);
  CHECK(memcmp(&j4, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY4_KEY), sizeof(j4)) ==
        0);
  CHECK(memcmp(&chapters, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
               sizeof(chapters)) == 0);
  const pref_entry_t* j5 = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY5_KEY);
  uint32_t tag = 0;
  h2_xianxia_rpg_save_t save;
  CHECK(j5 && j5->writes == 1 &&
        h2_xianxia_rpg_journey_decode(j5->value, j5->size, &save, &tag));
  CHECK(save.chapter == 5 && save.level == 8 && save.exp == 465 &&
        save.pills == 3);
  CHECK(save.flags & H2_XIANXIA_RPG_FLAG_C4_FAINT);
  CHECK(tag == (h2_xianxia_rpg_predecessor_tag(0, j2.value, j2.size) ^
                h2_xianxia_rpg_predecessor_tag(0, j3.value, j3.size) ^
                h2_xianxia_rpg_predecessor_tag(0, j4.value, j4.size)));
  for (unsigned state = 0; state < 7; ++state) {
    memset(&prefs, 0, sizeof(prefs));
    plant_released_v04();
    uint8_t bytes[32];
    save.chapter = state == 0 ? 6u : 5u;
    h2_xianxia_rpg_journey_encode(&save, 0, bytes);
    if (state == 1) bytes[0] = 3;
    if (state == 1) {
      uint32_t hash = 2166136261u ^ 0x4A524E59u;
      for (size_t i = 0; i < 28; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
      }
      for (unsigned i = 0; i < 4; ++i)
        bytes[28 + i] = (uint8_t)(hash >> (8u * i));
    }
    if (state == 2) bytes[28] ^= 1;
    pref_entry_t* e =
        pref_put(H2_XIANXIA_RPG_PREF_JOURNEY5_KEY,
                 state == 3 ? H2_PAL_PREF_ENTRY_U32 : H2_PAL_PREF_ENTRY_BLOB,
                 bytes, sizeof(bytes));
    e->writes = 0;
    if (state == 4) {
      prefs.read_error_key = H2_XIANXIA_RPG_PREF_JOURNEY5_KEY;
      prefs.read_error = H2_PAL_ERR_IO;
    }
    if (state == 5) {
      prefs.read_error_key = H2_XIANXIA_RPG_PREF_JOURNEY4_KEY;
      prefs.read_error = H2_PAL_ERR_IO;
    }
    if (state == 6) {
      pref_entry_t* p = pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
      p->value[0] = 3;
    }
    const pref_entry_t old = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY4_KEY),
                       p = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
    reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
    fx.exit_after = 90;
    b = &layout.layout.title_buttons[1];
    script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
    CHECK(run_once() == H2_PAL_OK && e->writes == 0 &&
          memcmp(e->value, bytes, sizeof(bytes)) == 0);
    CHECK(memcmp(&old, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY4_KEY),
                 sizeof(old)) == 0);
    CHECK(memcmp(&p, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY), sizeof(p)) ==
          0);
  }
  use_prefs = false;
}

/* Exact bytes from anonymously downloaded formal v0.5, freshly completed
 * with real mouse input: Lv9, three pills, one win, six turns. */
static void plant_released_v05(void) {
  static const uint8_t journey[32] = {
      0x02, 0x05, 0x1e, 0x09, 0x03, 0x0d, 0x08, 0x03, 0x0d, 0x0d, 0x04,
      0x00, 0x1c, 0x02, 0x00, 0x00, 0x03, 0x00, 0x00, 0xe4, 0x01, 0x00,
      0x06, 0x00, 0x9c, 0xd3, 0x4e, 0xcb, 0x23, 0x60, 0x2f, 0x9e};
  static const uint8_t chapters[168] = {
      [0] = 2,     [1] = 10,     [2] = 16,     [68] = 9,     [69] = 3,
      [72] = 0x1c, [73] = 2,     [76] = 3,     [79] = 0xe4,  [80] = 1,
      [82] = 6,    [164] = 0xa0, [165] = 0x44, [166] = 0xad, [167] = 0x77};
  pref_put(H2_XIANXIA_RPG_PREF_JOURNEY5_KEY, H2_PAL_PREF_ENTRY_BLOB, journey,
           sizeof(journey))
      ->writes = 0;
  pref_put(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY, H2_PAL_PREF_ENTRY_BLOB, chapters,
           sizeof(chapters))
      ->writes = 0;
}
static void test_sixth_chapter_keeps_released_records(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_released_v05();
  const pref_entry_t j5 = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY5_KEY),
                     chapters = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
  h2_xianxia_rpg_game_t layout;
  h2_xianxia_rpg_game_init(&layout, WIDTH, HEIGHT, 1, NULL);
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 210;
  const h2_xianxia_rpg_rect_t* b = &layout.layout.title_buttons[2];
  const h2_xianxia_rpg_rect_t* row = &layout.layout.chapter_rows[5];
  script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
  script_tap(80, row->x + row->w / 2, row->y + row->h / 2);
  script_tap(190, WIDTH / 2, HEIGHT / 2);
  CHECK(run_once() == H2_PAL_OK);
  CHECK(memcmp(&j5, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY5_KEY), sizeof(j5)) ==
        0);
  CHECK(memcmp(&chapters, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
               sizeof(chapters)) == 0);
  const pref_entry_t* j6 = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY6_KEY);
  h2_xianxia_rpg_save_t save;
  uint32_t tag = 0;
  CHECK(j6 && j6->writes == 1 &&
        h2_xianxia_rpg_journey6_decode(j6->value, j6->size, &save, &tag));
  CHECK(save.chapter == 6 && save.level == 9 && save.exp == 540 &&
        save.pills == 3 && save.facts == 0);
  CHECK((save.flags & H2_XIANXIA_RPG_FLAG_CARRY) == 0xe4000000u);
  CHECK(tag == (h2_xianxia_rpg_predecessor_tag(0, NULL, 0) ^
                h2_xianxia_rpg_predecessor_tag(0, j5.value, j5.size)));
  for (unsigned state = 0; state < 7; ++state) {
    memset(&prefs, 0, sizeof(prefs));
    plant_released_v05();
    uint8_t bytes[H2_XIANXIA_RPG_JOURNEY6_BYTES];
    h2_xianxia_rpg_journey6_encode(&save, tag, bytes);
    if (state == 0) bytes[1] = 7;
    if (state == 1) bytes[0] = 4;
    if (state <= 1) {
      uint32_t hash = 2166136261u ^ 0x4A524E59u;
      for (size_t i = 0; i < 32; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
      }
      for (unsigned i = 0; i < 4; ++i)
        bytes[32 + i] = (uint8_t)(hash >> (8u * i));
    }
    if (state == 2) bytes[32] ^= 1;
    pref_entry_t* e =
        pref_put(H2_XIANXIA_RPG_PREF_JOURNEY6_KEY,
                 state == 3 ? H2_PAL_PREF_ENTRY_U32 : H2_PAL_PREF_ENTRY_BLOB,
                 bytes, sizeof(bytes));
    e->writes = 0;
    if (state == 4 || state == 5) {
      prefs.read_error_key = state == 4 ? H2_XIANXIA_RPG_PREF_JOURNEY6_KEY
                                        : H2_XIANXIA_RPG_PREF_JOURNEY5_KEY;
      prefs.read_error = H2_PAL_ERR_IO;
    }
    if (state == 6) pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY)->value[0] = 4;
    const pref_entry_t old = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY5_KEY),
                       p = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
    reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
    fx.exit_after = 90;
    b = &layout.layout.title_buttons[1];
    script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
    CHECK(run_once() == H2_PAL_OK && e->writes == 0 &&
          memcmp(e->value, bytes, sizeof(bytes)) == 0);
    CHECK(memcmp(&old, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY5_KEY),
                 sizeof(old)) == 0);
    CHECK(memcmp(&p, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY), sizeof(p)) ==
          0);
  }
  use_prefs = false;
}

/* Captured from the public v0.6 zip (cf772905...cf707a), completed through
 * real Chromium pointer input in a fresh profile: cold route, five breaths.
 * These are original PAL blob bytes, not a re-encoded model fixture. */
static void plant_released_v06(void) {
  static const uint8_t journey[36] = {
      0x03, 0x06, 0x24, 0x09, 0x03, 0x10, 0x06, 0x03, 0x10, 0x06, 0x01, 0x00,
      0x1c, 0x02, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x10, 0x88, 0x92, 0xe6};
  static const uint8_t chapters[168] = {
      [0] = 2,      [1] = 10,     [2] = 32,     [84] = 9,    [85] = 3,
      [87] = 30,    [88] = 0x1c,  [89] = 2,     [92] = 0x54, [98] = 5,
      [164] = 0xf3, [165] = 0x12, [166] = 0xf3, [167] = 0xc2};
  pref_put(H2_XIANXIA_RPG_PREF_JOURNEY6_KEY, H2_PAL_PREF_ENTRY_BLOB, journey,
           sizeof(journey))
      ->writes = 0;
  pref_put(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY, H2_PAL_PREF_ENTRY_BLOB, chapters,
           sizeof(chapters))
      ->writes = 0;
}
static void test_seventh_chapter_keeps_formal_sixth(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_released_v06();
  const pref_entry_t old = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY6_KEY),
                     table = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
  h2_xianxia_rpg_game_t layout;
  h2_xianxia_rpg_game_init(&layout, WIDTH, HEIGHT, 1, NULL);
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 350;
  const h2_xianxia_rpg_rect_t* b = &layout.layout.title_buttons[1];
  script_tap(40, b->x + b->w / 2, b->y + b->h / 2); /* Formal ending. */
  script_tap(210, 320, 240);                        /* Direct onward. */
  script_tap(320, 320, 240); /* Start with inherited state. */
  CHECK(run_once() == H2_PAL_OK);
  CHECK(memcmp(&old, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY6_KEY),
               sizeof(old)) == 0);
  CHECK(memcmp(&table, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
               sizeof(table)) == 0);
  const pref_entry_t* j7 = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY7_KEY);
  uint32_t tag = 0;
  h2_xianxia_rpg_save_t save;
  CHECK(j7 && j7->writes == 1 &&
        h2_xianxia_rpg_journey7_decode(j7->value, j7->size, &save, &tag));
  CHECK(save.chapter == 7 && save.level == 9 && save.pills == 3 &&
        save.facts == 30 && save.flags == 0);
  CHECK(tag == h2_xianxia_rpg_predecessor_tag(0, old.value, old.size));
  for (unsigned state = 0; state < 8; ++state) {
    memset(&prefs, 0, sizeof(prefs));
    plant_released_v06();
    h2_xianxia_rpg_save_t damaged = save;
    if (state == 0) damaged.chapter = 8;
    if (state == 6) damaged.facts |= 1u << 20;
    uint8_t bytes[36];
    h2_xianxia_rpg_journey7_encode(&damaged, tag, bytes);
    if (state == 1) bytes[0] = 4;
    if (state == 2) bytes[32] ^= 1;
    pref_entry_t* e = pref_put(
        H2_XIANXIA_RPG_PREF_JOURNEY7_KEY,
        state == 3 ? H2_PAL_PREF_ENTRY_U32 : H2_PAL_PREF_ENTRY_BLOB, bytes, 36);
    e->writes = 0;
    if (state == 4 || state == 5) {
      prefs.read_error_key = state == 4 ? H2_XIANXIA_RPG_PREF_JOURNEY7_KEY
                                        : H2_XIANXIA_RPG_PREF_JOURNEY6_KEY;
      prefs.read_error = H2_PAL_ERR_IO;
    }
    if (state == 7) pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY)->value[0] = 4;
    const pref_entry_t sixth = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY6_KEY),
                       chapters = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
    reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
    fx.exit_after = 350;
    script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
    /* If Continue falls back to the sixth ending, automatic onward must
     * preserve unknown journey7 instead of replacing it from the recap. */
    if (state < 5 || state == 6) {
      script_tap(210, 320, 240);
      script_tap(320, 320, 240);
    }
    CHECK(run_once() == H2_PAL_OK && e->writes == 0 &&
          memcmp(e->value, bytes, 36) == 0);
    CHECK(memcmp(&sixth, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY6_KEY),
                 sizeof(sixth)) == 0);
    CHECK(memcmp(&chapters, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
                 sizeof(chapters)) == 0);
  }
  use_prefs = false;
}

/* Original PAL bytes from the public v0.7 zip (e9d2ddd5...43eebd0),
 * cleared in fresh Chromium through real pointer input: three right answers,
 * sword/guard mirror victory, then ending refresh. No new codec generated
 * this predecessor fixture. */
static void plant_seventh_clear(void) {
  static const uint8_t bytes[36] = {
      0x03, 0x07, 0x2c, 0x0a, 0x03, 0x14, 0x0b, 0x04, 0x14, 0x0b, 0x04, 0x03,
      0xb2, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11, 0x00,
      0x9c, 0xd3, 0x4e, 0xcb, 0xe8, 0x0f, 0x00, 0x00, 0x57, 0xc7, 0x13, 0x61};
  static const uint8_t table[168] = {
      [0] = 2,    [1] = 10,     [2] = 64,     [100] = 10,   [101] = 3,
      [102] = 3,  [103] = 15,   [104] = 0xb2, [105] = 2,    [112] = 1,
      [114] = 17, [164] = 0x1f, [165] = 0x28, [166] = 0x1a, [167] = 0xf9};
  pref_put(H2_XIANXIA_RPG_PREF_JOURNEY7_KEY, H2_PAL_PREF_ENTRY_BLOB, bytes,
           sizeof(bytes))
      ->writes = 0;
  pref_put(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY, H2_PAL_PREF_ENTRY_BLOB, table,
           sizeof(table))
      ->writes = 0;
}
static void test_eighth_record_ownership(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_seventh_clear();
  const pref_entry_t old = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY7_KEY),
                     table = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
  h2_xianxia_rpg_game_t layout;
  h2_xianxia_rpg_game_init(&layout, WIDTH, HEIGHT, 1, NULL);
  const h2_xianxia_rpg_rect_t* b = &layout.layout.title_buttons[1];
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 350;
  script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
  script_tap(210, 320, 240);
  script_tap(320, 320, 240);
  CHECK(run_once() == H2_PAL_OK);
  CHECK(memcmp(&old, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY7_KEY),
               sizeof(old)) == 0);
  CHECK(memcmp(&table, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
               sizeof(table)) == 0);
  const pref_entry_t* j8 = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY8_KEY);
  h2_xianxia_rpg_save_t save;
  uint32_t tag = 0;
  CHECK(j8 && j8->writes == 1 &&
        h2_xianxia_rpg_journey8_decode(j8->value, j8->size, &save, &tag));
  CHECK(save.chapter == 8 && save.level == 10 && save.pills == 3 &&
        save.trust == 3 && save.facts == 4072);
  CHECK(tag == (h2_xianxia_rpg_predecessor_tag(0, old.value, old.size) ^
                h2_xianxia_rpg_predecessor_tag(0, NULL, 0)));
  for (unsigned state = 0; state < 8; ++state) {
    memset(&prefs, 0, sizeof(prefs));
    plant_seventh_clear();
    h2_xianxia_rpg_save_t damaged = save;
    if (state == 0) damaged.chapter = 9;
    if (state == 6) damaged.facts |= 1u << 24;
    if (state == 7) damaged.flags |= 15u;
    uint8_t bytes[36];
    h2_xianxia_rpg_journey8_encode(&damaged, tag, bytes);
    if (state == 1) bytes[0] = 4;
    if (state == 2) bytes[32] ^= 1;
    pref_entry_t* e =
        pref_put(H2_XIANXIA_RPG_PREF_JOURNEY8_KEY,
                 state == 3 ? H2_PAL_PREF_ENTRY_U32 : H2_PAL_PREF_ENTRY_BLOB,
                 bytes, sizeof(bytes));
    e->writes = 0;
    if (state == 4 || state == 5) {
      prefs.read_error_key = state == 4 ? H2_XIANXIA_RPG_PREF_JOURNEY8_KEY
                                        : H2_XIANXIA_RPG_PREF_JOURNEY7_KEY;
      prefs.read_error = H2_PAL_ERR_IO;
    }
    const pref_entry_t seventh = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY7_KEY),
                       chapters = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
    reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
    fx.exit_after = 350;
    script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
    if (state != 5) {
      script_tap(210, 320, 240);
      script_tap(320, 320, 240);
    }
    CHECK(run_once() == H2_PAL_OK && e->writes == 0 &&
          memcmp(e->value, bytes, sizeof(bytes)) == 0);
    CHECK(memcmp(&seventh, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY7_KEY),
                 sizeof(seventh)) == 0);
    CHECK(memcmp(&chapters, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
                 sizeof(chapters)) == 0);
  }
  use_prefs = false;
}

/* Original PAL bytes from the independently accepted eighth-chapter WASM
 * (9cacf296144cdd16d735f0ceec800075aef95aaf), actual west-first play,
 * medicine treatment and Wen Zhou support. Kept raw, not made by this codec. */
static void plant_eighth_clear(void) {
  static const uint8_t bytes[36] = {
      0x03, 0x08, 0x32, 0x0b, 0x03, 0x15, 0x07, 0x06, 0x15, 0x07, 0x06, 0x00,
      0x57, 0x03, 0x00, 0x00, 0x9c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x08, 0xf8, 0xdf, 0x00, 0x63, 0x74, 0xb9, 0x6c};
  static const uint8_t table[168] = {
      [0] = 2,    [1] = 10,     [2] = 128,    [116] = 11,   [117] = 3,
      [119] = 31, [120] = 0x57, [121] = 3,    [124] = 0x9c, [128] = 1,
      [130] = 8,  [164] = 0xe6, [165] = 0x7a, [166] = 0xb5, [167] = 0x9c};
  pref_put(H2_XIANXIA_RPG_PREF_JOURNEY8_KEY, H2_PAL_PREF_ENTRY_BLOB, bytes,
           sizeof(bytes))
      ->writes = 0;
  pref_put(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY, H2_PAL_PREF_ENTRY_BLOB, table,
           sizeof(table))
      ->writes = 0;
}
static void test_ninth_record_ownership(void) {
  memset(&prefs, 0, sizeof(prefs));
  use_prefs = true;
  plant_eighth_clear();
  const pref_entry_t old = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY8_KEY),
                     table = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
  h2_xianxia_rpg_game_t layout;
  h2_xianxia_rpg_game_init(&layout, WIDTH, HEIGHT, 1, NULL);
  const h2_xianxia_rpg_rect_t* b = &layout.layout.title_buttons[1];
  reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
  fx.exit_after = 350;
  script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
  script_tap(210, 320, 240);
  script_tap(320, 320, 240);
  CHECK(run_once() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  CHECK(memcmp(&old, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY8_KEY),
               sizeof(old)) == 0);
  CHECK(memcmp(&table, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
               sizeof(table)) == 0);
  const pref_entry_t* j9 = pref_find(H2_XIANXIA_RPG_PREF_JOURNEY9_KEY);
  h2_xianxia_rpg_save_t save;
  uint32_t tag = 0;
  CHECK(j9 && j9->writes == 1 &&
        h2_xianxia_rpg_journey9_decode(j9->value, j9->size, &save, &tag));
  CHECK(save.chapter == 9 && save.level == 11 && save.pills == 3 &&
        save.trust == 0 &&
        save.facts == (0xDFF808u | H2_XIANXIA_RPG_FACT_C9_ACTIVE_WEN));
  CHECK(tag == h2_xianxia_rpg_predecessor_tag(0, old.value, old.size));
  for (unsigned state = 0; state < 8; ++state) {
    memset(&prefs, 0, sizeof(prefs));
    plant_eighth_clear();
    h2_xianxia_rpg_save_t damaged = save;
    if (state == 0) damaged.chapter = 10;
    if (state == 6) damaged.facts |= 1u << 28;
    if (state == 7) damaged.flags |= 16u; /* Trial without its door. */
    uint8_t bytes[36];
    h2_xianxia_rpg_journey9_encode(&damaged, tag, bytes);
    if (state == 1) bytes[0] = 4;
    if (state == 2) bytes[32] ^= 1;
    pref_entry_t* e =
        pref_put(H2_XIANXIA_RPG_PREF_JOURNEY9_KEY,
                 state == 3 ? H2_PAL_PREF_ENTRY_U32 : H2_PAL_PREF_ENTRY_BLOB,
                 bytes, sizeof(bytes));
    e->writes = 0;
    if (state == 4 || state == 5) {
      prefs.read_error_key = state == 4 ? H2_XIANXIA_RPG_PREF_JOURNEY9_KEY
                                        : H2_XIANXIA_RPG_PREF_JOURNEY8_KEY;
      prefs.read_error = H2_PAL_ERR_IO;
    }
    const pref_entry_t eighth = *pref_find(H2_XIANXIA_RPG_PREF_JOURNEY8_KEY),
                       chapters = *pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY);
    reset(WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565);
    fx.exit_after = 350;
    script_tap(40, b->x + b->w / 2, b->y + b->h / 2);
    if (state != 5) {
      script_tap(210, 320, 240);
      script_tap(320, 320, 240);
    }
    CHECK(run_once() == H2_PAL_OK && e->writes == 0 &&
          memcmp(e->value, bytes, sizeof(bytes)) == 0);
    CHECK(fx.touch.script_next == fx.touch.script_count);
    CHECK(memcmp(&eighth, pref_find(H2_XIANXIA_RPG_PREF_JOURNEY8_KEY),
                 sizeof(eighth)) == 0);
    CHECK(memcmp(&chapters, pref_find(H2_XIANXIA_RPG_PREF_CHAPTERS_KEY),
                 sizeof(chapters)) == 0);
  }
  use_prefs = false;
}

int main(void) {
  if (H2_XIANXIA_RPG_TEST_NINTH_RECORD_OWNERSHIP) {
    test_ninth_record_ownership();
    puts("xianxia_rpg chapter9 App tests passed");
    return 0;
  }
  if (H2_XIANXIA_RPG_TEST_EIGHTH_RECORD_OWNERSHIP) {
    puts("xianxia_rpg App group: chapter 8 record ownership (9 scenarios)");
    test_eighth_record_ownership();
    puts("xianxia_rpg chapter8 App tests passed");
  } else {
    puts("xianxia_rpg App group: lifecycle and chapters 1-7");
    test_seventh_chapter_keeps_formal_sixth();
    test_sixth_chapter_keeps_released_records();
    CHECK(h2_xianxia_rpg_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    test_plays_and_cleans_up();
    test_rejects_unusable_displays();
    test_ready_failure_ends_run();
    test_upgrade_keeps_v01_keys();
    test_corrupt_journey_is_kept();
    test_import_record_combinations();
    test_three_key_import_and_write_failures();
    test_third_chapter_keeps_released_journey();
    test_fourth_chapter_keeps_released_records();
    test_fifth_chapter_keeps_released_records();
    printf("xianxia_rpg app tests passed\n");
  }
  return 0;
}
