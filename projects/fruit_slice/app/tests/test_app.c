/* Run loop tests through the public entry with fake Display/Touch providers
 * and a Desktop-backed Runtime. */
#include "h2_desktop_platform.h"
#include "h2_fruit_slice.h"
#include "h2_fruit_slice_game.h"
#include "h2_pal_unsupported.h"
#include "h2_sqlite.h"
#include "input_clock.h"

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
#define SHADOW_STRIDE 800
#define SCRIPT_MAX 128

typedef struct fake_display {
  h2_display_info_t info;
  int open_result, draw_result, present_result;
  unsigned fail_from_frame;
  int opened, open_calls, close_calls;
  size_t draws, presents, rows_drawn, pixels_drawn, full_frames;
  uint32_t brightness;
  uint16_t shadow[SHADOW_STRIDE * 800];
} fake_display_t;

typedef struct fake_touch {
  h2_pal_touch_info_t info;
  int opened, open_calls, close_calls;
  h2_pal_result_t poll_error; /* Nonzero: every poll fails with it. */
  /* Scripted events delivered one per poll from frame first_frame on. */
  h2_pal_touch_event_t script[SCRIPT_MAX];
  unsigned script_frame[SCRIPT_MAX];
  size_t script_count, script_next;
  unsigned last_frame, error_frame, error_until;
  bool burst, latched, verified_down;
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
  h2_fruit_slice_language_t language;
  unsigned touch_from_frame;
  uint16_t probe_before;
  bool use_pref, fast_clock, layout_sweep;
  bool paced_clock, prewarm_audio, sink_started;
  uint32_t present_cost_ms, sink_queued_ms, sink_starved_ms, audio_open_cost_ms;
  uint64_t sink_updated_ms;
  unsigned sink_full_writes;
  unsigned autoplay_from, autoplay_until, autoplay_pause_from,
      autoplay_pause_until, reveal_until;
  uint64_t clock_ms;
  int clock_error;
  uint64_t clock_fail_after_ms;
  unsigned failure_count;
  bool failure_before_close;
  char failure_message[224];
  uint32_t seed;
  uint32_t earned_fruit_credit;
  const char* capture_name;
  unsigned capture_every, capture_from, capture_until;
  unsigned capture_frames[48], capture_count;
  unsigned header_from, header_check, header_changed, claim_check;
  size_t event_capacity;
  uint32_t drops, touch_errors;
  uint32_t header_hash;
  const h2_pal_pref_api_t* pref_override;
  const h2_pal_task_api_t* task_override;
  bool background_saves;
  void (*loop_hook)(void);
  bool use_audio, audio_active;
  unsigned audio_starts, audio_stops, audio_opens, audio_closes, audio_writes;
  unsigned pause_from, pause_until;
  int audio_close_result;
  unsigned audio_stop_failures, audio_block_writes, audio_successful_writes;
  h2_pal_audio_api_t audio_api;
  h2_pal_audio_track_t audio_track;
} fixture_t;

static fixture_t fx;

static int capture_log(void* user, h2_pal_log_level_t level, const char* scope,
                       const char* message) {
  (void)user;
  const char prefix[] = "H2_FRUIT_SLICE_FAILURE ";
  if (scope != NULL && !strcmp(scope, "fruit-slice") &&
      !strncmp(message, prefix, sizeof(prefix) - 1u)) {
    CHECK(level == H2_PAL_LOG_ERROR);
    ++fx.failure_count;
    (void)snprintf(fx.failure_message, sizeof(fx.failure_message), "%s",
                   message);
    fx.failure_before_close = fx.display.opened && fx.touch.opened;
  }
  return h2_pal_log_write(h2_desktop_platform_log_api(), level, scope, message);
}
static const h2_pal_log_vtable_t capture_log_vtable = {.write = capture_log};
static const h2_pal_log_api_t captured_log = {.vtable = &capture_log_vtable};

static struct {
  uint8_t saved[H2_FRUIT_SLICE_PROGRESS_BYTES];
  uint8_t staged[H2_FRUIT_SLICE_PROGRESS_BYTES];
  bool exists, pending;
  unsigned commits, failed_commits, reads, best_reads;
  uint32_t best_value;
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
  *out_value = pref.best_value != 0u ? pref.best_value : 21u;
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
  if (d->draw_result != H2_PAL_OK && fx.frames >= d->fail_from_frame)
    return d->draw_result;
  CHECK(format == H2_DISPLAY_PIXEL_RGB565);
  /* Partial rectangles start and end on even columns. */
  CHECK(rect->x >= 0 && rect->width > 0 && rect->x % 2 == 0 &&
        (rect->x + rect->width) % 2 == 0 &&
        rect->x + rect->width <= d->info.width);
  CHECK(rect->y >= 0 && rect->y + rect->height <= d->info.height);
  CHECK(stride == (size_t)d->info.width * 2u);
  CHECK(d->info.width <= SHADOW_STRIDE && d->info.height <= 800);
  for (int y = 0; y < rect->height; ++y)
    memcpy(&d->shadow[(size_t)(rect->y + y) * SHADOW_STRIDE + (size_t)rect->x],
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
  if (d->present_result != H2_PAL_OK && fx.frames >= d->fail_from_frame)
    return d->present_result;
  if (fx.paced_clock) fx.clock_ms += fx.present_cost_ms;
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
static h2_pal_result_t touch_open_pointer(void* user, bool* needs_close) {
  *needs_close = false;
  if (((fake_touch_t*)user)->opened) return H2_PAL_ERR_BUSY;
  return touch_open(user);
}
static h2_pal_result_t touch_info(void* user, h2_pal_touch_info_t* out) {
  fake_touch_t* t = user;
  if (!t->opened) return H2_PAL_ERR_INVALID_STATE;
  *out = t->info;
  return H2_PAL_OK;
}
static h2_pal_result_t touch_poll(void* user, h2_pal_touch_event_t* out) {
  fake_touch_t* t = user;
  CHECK(t->opened && input_thread);
  if (t->error_frame && fx.frames >= t->error_frame &&
      fx.frames <= (t->error_until ? t->error_until : t->error_frame))
    return H2_PAL_ERR_TIMEOUT;
  if (t->poll_error != H2_PAL_OK) return t->poll_error;
  /* Accelerated first-install regression: ordinary pointer strokes play
   * real rounds, then leave enough game time for misses to end the last run. */
  const unsigned autoplay_from = fx.autoplay_from ? fx.autoplay_from : 2u;
  if (fx.autoplay_until != 0u && fx.frames >= autoplay_from &&
      fx.frames < fx.autoplay_until && t->last_frame != fx.frames &&
      !(fx.frames >= fx.autoplay_pause_from &&
        fx.frames < fx.autoplay_pause_until)) {
    t->last_frame = fx.frames;
    const unsigned point = (fx.frames - autoplay_from) % 150u;
    const int32_t w = fx.display.info.width, h = fx.display.info.height;
    const int32_t x = point % 2u == 0u ? 0 : w;
    int32_t y = h * 7 / 12;
    if (point < 2u) {
      h2_fruit_slice_game_t layout;
      h2_fruit_slice_rect_t start;
      h2_fruit_slice_game_init(&layout, w, h, 42u, 0u);
      if (fx.frames - autoplay_from < 150u) {
        y = (int32_t)layout.fruits[0].body.y;
      } else {
        layout.phase = H2_FRUIT_SLICE_PHASE_GAME_OVER;
        CHECK(h2_fruit_slice_game_button_rect(
            &layout, H2_FRUIT_SLICE_BUTTON_START, &start));
        y = (int32_t)(start.y1 - H2_FRUIT_SLICE_MENU_SCALE *
                                     h2_fruit_slice_game_radius(
                                         &layout, H2_FRUIT_SLICE_ORANGE));
      }
    }
    *out = (h2_pal_touch_event_t){
        point % 2u == 0u ? H2_PAL_TOUCH_EVENT_DOWN : H2_PAL_TOUCH_EVENT_UP, x,
        y, 0};
    if (fx.layout_sweep && point >= 2u) {
      const unsigned leg = (point - 2u) % 6u;
      out->kind = leg == 0u   ? H2_PAL_TOUCH_EVENT_DOWN
                  : leg == 5u ? H2_PAL_TOUCH_EVENT_UP
                              : H2_PAL_TOUCH_EVENT_MOVE;
      out->x = leg % 2u == 0u ? 0 : w;
      out->y = (int32_t)((float)h * (0.25f + 0.10f * (float)leg));
    }
    /* Map layout-space strokes to the configured Touch coordinate range. */
    out->x = out->x * fx.touch.info.width / w;
    out->y = out->y * fx.touch.info.height / h;
    return H2_PAL_OK;
  }
  /* Finish naturally earned reveals after autoplay stops, without slicing
   * the restart fruit or changing the economy controls that follow. */
  if (fx.autoplay_until != 0u && fx.frames >= fx.autoplay_until &&
      (fx.frames < (fx.reveal_until ? fx.reveal_until : fx.touch_from_frame) ||
       (fx.reveal_until == 0u && t->script_next == t->script_count)) &&
      t->last_frame != fx.frames) {
    t->last_frame = fx.frames;
    *out = (h2_pal_touch_event_t){
        fx.frames % 2u == 0u ? H2_PAL_TOUCH_EVENT_DOWN : H2_PAL_TOUCH_EVENT_UP,
        5, 100, 0};
    return H2_PAL_OK;
  }
  /* One event per frame keeps strokes spread over game time. */
  if (fx.frames < fx.touch_from_frame ||
      (!t->burst && t->last_frame == fx.frames) ||
      t->script_next == t->script_count ||
      fx.frames < t->script_frame[t->script_next])
    return H2_PAL_ERR_WOULD_BLOCK;
  if (fx.autoplay_until != 0u && fx.pref_override != NULL &&
      fx.frames == fx.touch_from_frame) {
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
static h2_pal_result_t touch_pointer(void* user,
                                     h2_pal_touch_sample_t* sample) {
  fake_touch_t* t = user;
  const h2_pal_result_t rc = touch_poll(user, &sample->event);
  sample->needs_reopen = t->latched && rc == H2_PAL_ERR_IO;
  sample->fresh_down = t->verified_down && rc == H2_PAL_OK &&
                       sample->event.kind == H2_PAL_TOUCH_EVENT_DOWN;
  return rc;
}
static h2_pal_result_t touch_close(void* user) {
  fake_touch_t* t = user;
  ++t->close_calls;
  t->opened = 0;
  return H2_PAL_OK;
}
static const h2_pal_touch_vtable_t touch_vtable = {
    .open = touch_open,
    .get_info = touch_info,
    .poll_event = touch_poll,
    .close = touch_close,
    .poll_pointer = touch_pointer,
    .open_pointer = touch_open_pointer,
    .close_pointer = touch_close};

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
  if (f->paced_clock) {
    const uint64_t elapsed =
        f->sink_started ? f->clock_ms - f->sink_updated_ms : 0u;
    f->sink_started = true;
    f->sink_updated_ms = f->clock_ms;
    if (elapsed > f->sink_queued_ms) {
      f->sink_starved_ms += (uint32_t)(elapsed - f->sink_queued_ms);
      f->sink_queued_ms = 0u;
    } else {
      f->sink_queued_ms -= (uint32_t)elapsed;
    }
    if (f->sink_queued_ms > 20u) {
      ++f->sink_full_writes;
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    f->sink_queued_ms += 20u;
  }
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
  if (f->paced_clock) f->clock_ms += f->audio_open_cost_ms;
  f->sink_started = false;
  f->sink_queued_ms = 0u;
  f->sink_updated_ms = f->clock_ms;
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
      const uint16_t c = f->display.shadow[y * SHADOW_STRIDE + x];
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
  bool requested = false;
  if (f->frames == f->header_from || f->frames == f->header_check ||
      f->frames == f->header_changed) {
    uint32_t hash = 2166136261u;
    for (int y = 0; y < 20; ++y)
      for (int x = 0; x < f->display.info.width; ++x)
        hash = (hash ^ f->display.shadow[y * SHADOW_STRIDE + x]) * 16777619u;
    if (f->frames == f->header_from) f->header_hash = hash;
    if (f->header_changed && f->frames == f->header_changed)
      CHECK(hash != f->header_hash);
    if (f->header_check != 0u && f->frames == f->header_check)
      CHECK(hash ==
            f->header_hash); /* Claim leaves the Tasks heading visible. */
  }
  if (f->claim_check != 0u && f->frames == f->claim_check) {
    h2_fruit_slice_progress_t saved;
    CHECK(
        h2_fruit_slice_progress_decode(&saved, pref.saved, sizeof(pref.saved)));
    CHECK(saved.coins == 40u && saved.score_task == 2u);
  }
  for (unsigned i = 0; i < f->capture_count; ++i)
    requested |= f->frames == f->capture_frames[i];
  if (requested || f->frames == f->exit_after || f->frames == 1u ||
      (f->capture_every && f->frames >= f->capture_from && f->frames &&
       f->frames <= f->capture_until && f->frames % f->capture_every == 0u))
    capture_frame(f);
  if (f->frames == 3u)
    f->probe_before = f->display.shadow[288u * SHADOW_STRIDE + 240u];
  if (f->loop_hook != NULL) f->loop_hook();
  if (f->frames++ >= f->exit_after) return true;
  input_tick();
  return false;
}

static h2_pal_result_t ready(void* user) {
  fixture_t* f = user;
  ++f->ready_calls;
  /* The first frame is complete and visible before readiness. */
  CHECK(f->display.presents == 1u);
  CHECK(f->display.rows_drawn == (size_t)f->display.info.height);
  if (f->prewarm_audio) {
    CHECK(f->audio_opens == 1u && f->audio_writes == 0u);
    CHECK(f->clock_ms == f->audio_open_cost_ms + f->present_cost_ms);
  }
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
  fx.seed = 42u;
  fx.touch_from_frame = UINT32_MAX;
}

static h2_pal_result_t accelerated_clock(void* user, uint64_t* out_ms) {
  (void)user;
  if (fx.clock_error != H2_PAL_OK && fx.clock_ms >= fx.clock_fail_after_ms)
    return fx.clock_error;
  if (!fx.paced_clock && !input_thread) fx.clock_ms += 10u;
  *out_ms = fx.clock_ms;
  return H2_PAL_OK;
}

/* Model the real SDK's early tick timeout, without sleeping the host. */
static h2_pal_result_t paced_recv(void* user, h2_pal_queue_t* queue, void* item,
                                  uint32_t timeout_ms) {
  const h2_pal_queue_api_t* real = h2_desktop_platform_queue_api();
  const h2_pal_result_t rc = real->vtable->recv(user, queue, item, 0u);
  if ((rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) &&
      timeout_ms) {
    CHECK(timeout_ms != UINT32_MAX);
    const uint32_t ticks = timeout_ms / 10u;
    fx.clock_ms += ticks ? ticks * 10u : 1u;
    return H2_PAL_ERR_TIMEOUT;
  }
  return rc;
}

static h2_pal_result_t run(void) {
  const h2_pal_queue_api_t* real_queue = h2_desktop_platform_queue_api();
  h2_pal_queue_vtable_t queue_vtable = *real_queue->vtable;
  queue_vtable.recv = paced_recv;
  const h2_pal_queue_api_t queue_api = {real_queue->user, &queue_vtable};
  const h2_pal_time_api_t* real_time = h2_desktop_platform_time_api();
  h2_pal_time_vtable_t time_vtable = *real_time->vtable;
  time_vtable.get_monotonic_ms = accelerated_clock;
  const h2_pal_time_api_t time_api = {.user = real_time->user,
                                      .vtable = &time_vtable};
  h2_runtime_config_t config = {0};
  config.event_queue_capacity = fx.event_capacity;
  config.board = "fruit-slice-test";
  config.target = "desktop";
  config.chip = "host";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = &fx.mem_api;
  config.log = &captured_log;
  config.time = fx.fast_clock || fx.paced_clock ? &time_api : real_time;
  config.timer = h2_pal_unsupported_timer_api();
  config.task = fx.task_override != NULL ? fx.task_override
                                         : h2_desktop_platform_task_api();
  config.queue = fx.paced_clock ? &queue_api : real_queue;
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
  input_clock_install(&config);
  CHECK(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  const h2_fruit_slice_config_t game = {.seed = fx.seed,
                                        .brightness_percent = 80u,
                                        .frame_period_ms = fx.paced_clock ? 25u
                                                           : fx.fast_clock
                                                               ? 1u
                                                               : 10u,
                                        .ready = ready,
                                        .should_exit = should_exit,
                                        .is_paused = is_paused,
                                        .language = fx.language,
                                        .prewarm_audio = fx.prewarm_audio,
                                        .background_saves = fx.background_saves,
                                        .user = &fx};
  const h2_pal_result_t rc = h2_fruit_slice_run(runtime, &game);
  CHECK(h2_runtime_dropped_event_count(runtime, &fx.drops) == H2_PAL_OK);
  h2_pal_result_t last_touch_error;
  CHECK(h2_runtime_input_touch_errors(runtime, &fx.touch_errors,
                                      &last_touch_error) == H2_PAL_OK);
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
  CHECK(fx.display.shadow[0] != fx.display.shadow[288u * SHADOW_STRIDE + 240u]);
}

static void test_scaled_touch_slices_title_melon(void) {
  reset(70u);
  /* Touch reports half the display resolution. */
  fx.touch.info = (h2_pal_touch_info_t){WIDTH / 2, HEIGHT / 2};
  fx.touch_from_frame = 5u;
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 30, 144, 0};
  for (int x = 60; x <= 210; x += 30)
    fx.touch.script[fx.touch.script_count++] =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, x, 144, 0};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 210, 144, 0};
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  /* The melon at the display center is gone once its halves fell away. */
  CHECK(fx.display.shadow[288u * SHADOW_STRIDE + 240u] != fx.probe_before);
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
  reset(120u);
  fx.paced_clock = true;
  fx.touch.poll_error = H2_PAL_ERR_IO;
  fx.touch.latched = true;
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.display.presents == 120u && fx.failure_count == 0u);
  CHECK(fx.touch.open_calls > 1 && fx.touch.close_calls == fx.touch.open_calls);
}

/* Losing a release must cancel even with no more device reports. A device
 * error may be followed by a synthetic DOWN for the still-held pointer. */
static void test_interrupted_tap(bool queue_full) {
  reset(14u);
  fx.paced_clock = true;
  fx.touch_from_frame = 3u;
  fx.header_from = 2u;
  fx.header_check = 8u;
  fx.header_changed = 13u;
  fx.event_capacity = queue_full ? 1u : 4u;
  fx.touch.burst = true;
  fx.touch.error_frame = queue_full ? 0u : 4u;
  fx.touch.script[0] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 120, 450, 3};
  fx.touch.script_frame[0] = 3u;
  fx.touch.script_count = 1u;
  if (!queue_full) {
    /* A foreign release cannot re-arm the interrupted finger. */
    fx.touch.script[1] =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 120, 450, 9};
    fx.touch.script_frame[1] = 5u;
    fx.touch.script[2] = fx.touch.script[0];
    fx.touch.script_frame[2] = 6u;
    fx.touch.script_count = 3u;
  }
  size_t i = fx.touch.script_count++;
  fx.touch.script[i] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 120, 450, 3};
  fx.touch.script_frame[i] = queue_full ? 3u : 7u;
  fx.touch.script[fx.touch.script_count] = fx.touch.script[0];
  fx.touch.script_frame[fx.touch.script_count++] = 10u;
  fx.touch.script[fx.touch.script_count] = fx.touch.script[i];
  fx.touch.script_frame[fx.touch.script_count++] = 11u;
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count &&
        fx.drops == (queue_full ? 1u : 0u));
}

static void test_consumed_device_release(void) {
  reset(42u);
  fx.paced_clock = true;
  fx.header_from = 2u;
  fx.header_check = 36u;
  fx.header_changed = 41u;
  fx.touch_from_frame = 3u;
  fx.touch.burst = fx.touch.verified_down = true;
  fx.touch.error_frame = 4u;
  fx.touch.error_until = 33u;
  fx.touch.script[0] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 120, 450, 3};
  fx.touch.script_frame[0] = 3u;
  /* Recovery confirmed and consumed the old release before this poll. */
  fx.touch.script[1] = fx.touch.script[0];
  fx.touch.script_frame[1] = 38u;
  fx.touch.script[2] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 120, 450, 3};
  fx.touch.script_frame[2] = 38u;
  fx.touch.script_count = 3u;
  CHECK(run() == H2_PAL_OK);
  /* The Tasks page is static after the verified new tap. Every title frame
   * during the 30-error window must still have presented. */
  CHECK(fx.frames == 43u && fx.display.presents >= 36u &&
        fx.touch_errors == 30u);
  CHECK(fx.touch.script_next == 3u);
  CHECK(fx.touch.open_calls == 1 && fx.touch.close_calls == 1);
}

static void test_refused_move_with_two_contacts(void) {
  reset(16u);
  fx.paced_clock = fx.touch.burst = true;
  fx.event_capacity = 1u;
  fx.touch_from_frame = 3u;
  fx.header_from = 2u;
  fx.header_check = 10u;
  fx.header_changed = 15u;
  const h2_pal_touch_event_t points[] = {
      {H2_PAL_TOUCH_EVENT_DOWN, 120, 450, 9},
      {H2_PAL_TOUCH_EVENT_MOVE, 121, 450, 9}, /* Same burst, full queue. */
      {H2_PAL_TOUCH_EVENT_UP, 120, 450, 3},
      {H2_PAL_TOUCH_EVENT_DOWN, 120, 450, 9}, /* Ambiguous held sample. */
      {H2_PAL_TOUCH_EVENT_UP, 120, 450, 9},
      {H2_PAL_TOUCH_EVENT_DOWN, 120, 450, 9}, /* Fresh physical tap. */
      {H2_PAL_TOUCH_EVENT_UP, 120, 450, 9},
  };
  const unsigned frames[] = {3, 3, 5, 6, 7, 11, 12};
  fx.touch.script_count = sizeof(points) / sizeof(points[0]);
  memcpy(fx.touch.script, points, sizeof(points));
  memcpy(fx.touch.script_frame, frames, sizeof(frames));
  CHECK(run() == H2_PAL_OK && fx.touch.script_next == fx.touch.script_count);
  CHECK(!fx.touch_errors && fx.drops == 1u);
}

static void test_interleaved_contacts(void) {
  reset(23u);
  fx.touch.info = (h2_pal_touch_info_t){WIDTH / 2, HEIGHT / 2};
  fx.touch_from_frame = 3u;
  fx.header_from = 2u;
  fx.header_check = 17u;
  fx.header_changed = 22u;
  const h2_pal_touch_event_t points[] = {
      {H2_PAL_TOUCH_EVENT_DOWN, 20, 144, 3}, /* Left of title melon. */
      {H2_PAL_TOUCH_EVENT_DOWN, 210, 144, 9},
      {H2_PAL_TOUCH_EVENT_MOVE, 220, 144, 9},
      {H2_PAL_TOUCH_EVENT_UP, 220, 144, 9}, /* No slice between fingers. */
      {H2_PAL_TOUCH_EVENT_UP, 20, 144, 3},
      {H2_PAL_TOUCH_EVENT_DOWN, 60, 225, 9}, /* Tasks button. */
      {H2_PAL_TOUCH_EVENT_DOWN, 60, 225, 3},
      {H2_PAL_TOUCH_EVENT_UP, 60, 225, 3}, /* Cannot click for ID 9. */
      {H2_PAL_TOUCH_EVENT_CANCEL, 60, 225, 9},
      {H2_PAL_TOUCH_EVENT_UP, 60, 225, 9},
      {H2_PAL_TOUCH_EVENT_DOWN, 60, 225, 3},
      {H2_PAL_TOUCH_EVENT_CANCEL, 0, 0, H2_PAL_TOUCH_ALL_POINTERS},
      {H2_PAL_TOUCH_EVENT_UP, 60, 225, 3},
      {H2_PAL_TOUCH_EVENT_DOWN, 60, 225, 3}, /* Same ID, new stroke. */
      {H2_PAL_TOUCH_EVENT_UP, 60, 225, 3},
  };
  fx.touch.script_count = sizeof(points) / sizeof(points[0]);
  memcpy(fx.touch.script, points, sizeof(points));
  fx.touch.script_frame[13] = 18u;
  CHECK(run() == H2_PAL_OK && fx.touch.script_next == fx.touch.script_count);
  CHECK(!fx.touch_errors && !fx.drops && fx.touch.close_calls == 1);
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
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, x, y, 0};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, x, y, 0};
}

static void script_tap_at(unsigned frame, int32_t x, int32_t y) {
  const size_t next = fx.touch.script_count;
  script_tap(x, y);
  fx.touch.script_frame[next] = frame;
  fx.touch.script_frame[next + 1u] = frame + 1u;
}

static void test_progress_persists_across_sessions(void) {
  memset(&pref, 0, sizeof(pref));
  const h2_fruit_slice_progress_t initial = {.coins = 80u,
                                             .fruit_credit = 10u,
                                             .combo_credit = 0u,
                                             .score_task = 1u,
                                             .owned = 1u,
                                             .equipped = 0u};
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
  const h2_fruit_slice_progress_t initial = {.coins = 72u,
                                             .fruit_credit = 10u,
                                             .combo_credit = 2u,
                                             .score_task = 2u,
                                             .owned = 3u,
                                             .equipped = 1u};
  h2_fruit_slice_progress_encode(&initial, pref.saved);
  pref.exists = true;
  for (unsigned failure = 0; failure < 2u; ++failure) {
    if (failure == 0u)
      pref.read_result = H2_PAL_ERR_IO;
    else {
      pref.read_result = H2_PAL_OK;
      pref.saved[4] = 3u; /* A newer format must never be overwritten. */
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
  fx.touch_from_frame = 1506u; /* After the previous reveal tap's real UP. */
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
    bytes[invalid == 0u ? 4u : 0u] = invalid == 0u ? 3u : 0u;
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

static void test_paced_audio_through_public_entry(bool prewarm) {
  reset(200u);
  fx.prewarm_audio = prewarm;
  fx.audio_open_cost_ms = prewarm ? 500u : 0u;
  fx.display.info.height = 320;
  fx.touch.info.height = 320;
  fx.paced_clock = fx.use_audio = true;
  fx.present_cost_ms = 8u;
  fx.touch_from_frame = 5u;
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 20, 160, 0};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 20, 160, 0};
  CHECK(run() == H2_PAL_OK);
  printf(
      "public-entry simulated 480x320: elapsed=%llu starvation=%u full=%u writes=%u\n",
      (unsigned long long)fx.clock_ms, fx.sink_starved_ms, fx.sink_full_writes,
      fx.audio_successful_writes);
  CHECK(fx.clock_ms == 200u * 25u + fx.audio_open_cost_ms);
  CHECK(fx.sink_starved_ms == 0u && fx.sink_full_writes == 0u);
  CHECK(fx.audio_opens == 1u && fx.audio_closes == 1u);
}

static void test_audio_gesture_pause_and_exit(void) {
  reset(40u);
  fx.use_audio = true;
  fx.pause_from = 20u;
  fx.pause_until = 24u;
  fx.touch_from_frame = 5u;
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 60, 288, 0};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 400, 288, 0};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 400, 288, 0};
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
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 60, 288, 0};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 400, 288, 0};
  fx.touch.script[fx.touch.script_count++] =
      (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 400, 288, 0};
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
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 60, 288, 0};
    fx.touch.script[fx.touch.script_count++] =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_MOVE, 400, 288, 0};
    fx.touch.script[fx.touch.script_count++] =
        (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 400, 288, 0};
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
    /* Variety changes deterministic wave choices. 520 frames reaches the
     * weapon's 10-point duplicate as well as the first bomb with seed 42. */
    reset(520u);
    fx.pref_override = &recovering_api;
    fx.fast_clock = true;
    fx.autoplay_until = 520u;
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

static void script_control(h2_fruit_slice_game_t* ui, uint32_t button) {
  h2_fruit_slice_rect_t r;
  CHECK(h2_fruit_slice_game_button_rect(ui, button, &r));
  const int32_t x = (int32_t)((r.x0 + r.x1) * 0.5f);
  const int32_t y = (int32_t)((r.y0 + r.y1) * 0.5f);
  script_tap(x, y);
  h2_fruit_slice_game_pointer(ui, H2_FRUIT_SLICE_POINTER_DOWN, (float)x,
                              (float)y);
  h2_fruit_slice_game_pointer(ui, H2_FRUIT_SLICE_POINTER_UP, (float)x,
                              (float)y);
}

static void script_visit(h2_fruit_slice_game_t* ui, uint32_t button) {
  h2_fruit_slice_rect_t r;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_TASKS; ++i) {
    if (h2_fruit_slice_game_button_rect(ui, button, &r)) return;
    h2_fruit_slice_progress_layout_t layout;
    h2_fruit_slice_game_progress_layout(ui, &layout);
    const uint32_t base = ui->phase == H2_FRUIT_SLICE_PHASE_TASKS
                              ? H2_FRUIT_SLICE_BUTTON_CLAIM
                              : H2_FRUIT_SLICE_BUTTON_WEAPON;
    script_control(ui, button < base + layout.first
                           ? H2_FRUIT_SLICE_BUTTON_PREVIOUS
                           : H2_FRUIT_SLICE_BUTTON_NEXT);
  }
  CHECK(false);
}

static h2_fruit_slice_progress_t sqlite_progress(h2_sqlite_t* storage,
                                                 size_t expected_size) {
  h2_pal_pref_namespace_t* ns = NULL;
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
  void* data = NULL;
  size_t size = 0u;
  CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(), "progress_v1",
                     &data, &size) == H2_PAL_OK);
  CHECK(size == expected_size);
  h2_fruit_slice_progress_t result = {0};
  CHECK(h2_fruit_slice_progress_decode(&result, data, size));
  h2_pal_mem_free(h2_desktop_platform_default_allocator(), data);
  CHECK(ns->close(ns) == H2_PAL_OK);
  return result;
}

static void test_bilingual_last_page_claim_and_purchase(void) {
  const char* temporary = getenv("TEST_TMPDIR");
  CHECK(temporary != NULL);
  const int sizes[][2] = {{160, 160}, {240, 240}, {480, 320}};
  for (unsigned variant = 0; variant < 3u; ++variant) {
    for (unsigned language = 0; language < 2u; ++language) {
      char database[512], capture[64];
      (void)snprintf(database, sizeof(database), "%s/last-page-%u-%u.sqlite",
                     temporary, variant, language);
      const h2_sqlite_config_t config = {.path = database};
      h2_sqlite_t* storage = NULL;
      CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
      h2_fruit_slice_game_t ui;
      h2_fruit_slice_game_init(&ui, sizes[variant][0], sizes[variant][1], 1u,
                               141u);
      ui.language = (h2_fruit_slice_language_t)language;
      ui.progress.coins = 100u;
      ui.progress.challenge_credit[H2_FRUIT_SLICE_TASK_CLASSIC_COMBO -
                                   H2_FRUIT_SLICE_TASK_VARIETY] = 1u;
      uint8_t bytes[H2_FRUIT_SLICE_PROGRESS_BYTES];
      h2_fruit_slice_progress_encode(&ui.progress, bytes);
      h2_pal_pref_namespace_t* ns = NULL;
      CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                             H2_FRUIT_SLICE_PREF_NAMESPACE,
                             H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
      CHECK(ns->set_blob(ns, "progress_v1", bytes, sizeof(bytes)) == H2_PAL_OK);
      CHECK(ns->commit(ns) == H2_PAL_OK && ns->close(ns) == H2_PAL_OK);
      reset(60u);
      fx.display.info.width = fx.touch.info.width = (uint32_t)sizes[variant][0];
      fx.display.info.height = fx.touch.info.height =
          (uint32_t)sizes[variant][1];
      fx.pref_override = h2_sqlite_pref_api(storage);
      fx.fast_clock = true;
      fx.touch_from_frame = 2u;
      fx.language = (h2_fruit_slice_language_t)language;
      (void)snprintf(capture, sizeof(capture), "last-page-%s-%dx%d",
                     language == H2_FRUIT_SLICE_LANGUAGE_EN ? "en" : "zh",
                     sizes[variant][0], sizes[variant][1]);
      fx.capture_name = capture;
      script_control(&ui, H2_FRUIT_SLICE_BUTTON_TASKS);
      script_visit(
          &ui, H2_FRUIT_SLICE_BUTTON_CLAIM + H2_FRUIT_SLICE_TASK_CLASSIC_COMBO);
      script_control(
          &ui, H2_FRUIT_SLICE_BUTTON_CLAIM + H2_FRUIT_SLICE_TASK_CLASSIC_COMBO);
      script_control(
          &ui, H2_FRUIT_SLICE_BUTTON_CLAIM + H2_FRUIT_SLICE_TASK_CLASSIC_COMBO);
      script_control(&ui, H2_FRUIT_SLICE_BUTTON_BACK);
      script_control(&ui, H2_FRUIT_SLICE_BUTTON_SHOP);
      script_visit(&ui,
                   H2_FRUIT_SLICE_BUTTON_WEAPON + H2_FRUIT_SLICE_WEAPON_STORM);
      script_control(
          &ui, H2_FRUIT_SLICE_BUTTON_WEAPON + H2_FRUIT_SLICE_WEAPON_STORM);
      CHECK(run() == H2_PAL_OK);
      CHECK(fx.touch.script_next == fx.touch.script_count);
      h2_sqlite_destroy(storage);
      CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
      const h2_fruit_slice_progress_t saved =
          sqlite_progress(storage, H2_FRUIT_SLICE_PROGRESS_BYTES);
      CHECK(saved.coins == 20u &&
            saved.equipped == H2_FRUIT_SLICE_WEAPON_STORM &&
            saved.owned == 33u);
      CHECK(saved.challenge_credit[3] == 0u);
      h2_sqlite_destroy(storage);
      CHECK(remove(database) == 0);
    }
  }
}

static void test_variety_actual_frames(void) {
  const int sizes[][2] = {{480, 320}, {160, 160}};
  for (unsigned size = 0; size < 2u; ++size) {
    uint32_t expected_slices = 0u;
    for (unsigned language = 0; language < 2u; ++language) {
      memset(&pref, 0, sizeof(pref));
      reset(1200u);
      fx.use_pref = true;
      fx.display.info.width = fx.touch.info.width = sizes[size][0];
      fx.display.info.height = fx.touch.info.height = sizes[size][1];
      fx.language = (h2_fruit_slice_language_t)language;
      fx.fast_clock = true;
      fx.autoplay_until = 1200u;
      /* Let a normal mid-run wave rise intact before resuming real strokes. */
      fx.autoplay_pause_from = 580u;
      fx.autoplay_pause_until = 610u;
      char name[96];
      (void)snprintf(name, sizeof(name), "variety-actual-app-%s-%dx%d",
                     language ? "en" : "zh", sizes[size][0], sizes[size][1]);
      fx.capture_name = name;
      fx.capture_from = 20u;
      fx.capture_until = 1200u;
      fx.capture_every = 20u;
      CHECK(run() == H2_PAL_OK);
      CHECK(fx.display.presents > 100u && fx.allocations == fx.frees);
      h2_fruit_slice_progress_t earned;
      CHECK(pref.exists && h2_fruit_slice_progress_decode(&earned, pref.saved,
                                                          sizeof(pref.saved)));
      CHECK(earned.fruit_credit >= 20u);
      if (language == 0u)
        expected_slices = earned.fruit_credit;
      else
        CHECK(earned.fruit_credit == expected_slices);
    }
  }
}

static void test_020_sqlite_profile_upgrade(void) {
  const char* temporary = getenv("TEST_TMPDIR");
  CHECK(temporary != NULL);
  /* Real original v1 bytes: 500 coins, 20 fruit, 3 combos, already-claimed
   * 20-point task, all old weapons, flame equipped. No v2 serializer involved.
   */
  const uint8_t old_progress[32] = {
      0x46, 0x53, 0x50, 0x31, 1, 0, 0, 0, 0xf4, 1, 0, 0, 20, 0, 0, 0,
      3,    0,    0,    0,    2, 0, 0, 0, 7,    0, 0, 0, 2,  0, 0, 0};
  const uint8_t old_cards[32] = {0x46, 0x53, 0x43, 0x31, 1, 0, 0, 0, 2, 0, 0,
                                 0,    16,   0,    0,    0, 5, 2, 0, 0, 0, 0,
                                 1,    2,    3,    0,    0, 0, 0, 0, 0, 0};
  /* Check this immutable v1 fixture against the current unchanged card wire
   * decoder before any App run, rather than accepting a silently new format. */
  h2_fruit_slice_cards_t collection;
  h2_fruit_slice_cards_init(&collection, 480, 480, 1u);
  CHECK(h2_fruit_slice_cards_decode(&collection, old_cards, sizeof(old_cards)));
  const int sizes[][2] = {{160, 160}, {240, 240}, {480, 320}};
  for (unsigned scenario = 0; scenario < 6u; ++scenario) {
    const unsigned variant = scenario % 3u;
    const h2_fruit_slice_language_t language =
        scenario < 3u ? H2_FRUIT_SLICE_LANGUAGE_ZH_CN
                      : H2_FRUIT_SLICE_LANGUAGE_EN;
    char database[512];
    (void)snprintf(database, sizeof(database), "%s/020-upgrade-%u.sqlite",
                   temporary, scenario);
    const h2_sqlite_config_t config = {.path = database};
    h2_sqlite_t* storage = NULL;
    CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
    h2_pal_pref_namespace_t* ns = NULL;
    CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                           H2_FRUIT_SLICE_PREF_NAMESPACE,
                           H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
    uint8_t legacy[32];
    memcpy(legacy, old_progress, sizeof(legacy));
    /* All old claimed states and equipment combinations survive migration. */
    legacy[20] = (uint8_t)variant;
    legacy[24] = (uint8_t)((1u << (variant + 1u)) - 1u);
    legacy[28] = (uint8_t)variant;
    CHECK(ns->set_blob(ns, "progress_v1", legacy, sizeof(legacy)) == H2_PAL_OK);
    CHECK(ns->set_blob(ns, "cards_v1", old_cards, sizeof(old_cards)) ==
          H2_PAL_OK);
    CHECK(ns->set_u32(ns, "best", 141u) == H2_PAL_OK);
    CHECK(ns->set_u32(ns, "audio_v1", 0x41550103u) == H2_PAL_OK);
    CHECK(ns->commit(ns) == H2_PAL_OK && ns->close(ns) == H2_PAL_OK);
    reset(4u);
    fx.pref_override = h2_sqlite_pref_api(storage);
    fx.fast_clock = true;
    CHECK(run() == H2_PAL_OK);
    h2_fruit_slice_progress_t restored = sqlite_progress(storage, 32u);
    CHECK(restored.coins == 500u && restored.fruit_credit == 20u &&
          restored.combo_credit == 3u);
    CHECK(restored.score_task == variant &&
          restored.owned == (1u << (variant + 1u)) - 1u &&
          restored.equipped == variant);
    for (unsigned i = 0; i < H2_FRUIT_SLICE_CHALLENGES; ++i)
      CHECK(restored.challenge_credit[i] == 0u);

    reset(40u);
    fx.display.info.width = fx.touch.info.width = (uint32_t)sizes[variant][0];
    fx.display.info.height = fx.touch.info.height = (uint32_t)sizes[variant][1];
    fx.pref_override = h2_sqlite_pref_api(storage);
    fx.fast_clock = true;
    fx.use_audio = true;
    fx.language = language;
    fx.touch_from_frame = 2u;
    char capture[64];
    (void)snprintf(capture, sizeof(capture), "progression-%s-%dx%d",
                   language == H2_FRUIT_SLICE_LANGUAGE_EN ? "en" : "zh",
                   sizes[variant][0], sizes[variant][1]);
    fx.capture_name = capture;
    fx.capture_every = 2u;
    h2_fruit_slice_game_t ui;
    h2_fruit_slice_game_init(&ui, sizes[variant][0], sizes[variant][1], 1u,
                             141u);
    ui.language = language;
    ui.progress = restored;
    script_control(&ui, H2_FRUIT_SLICE_BUTTON_TASKS);
    script_visit(&ui, H2_FRUIT_SLICE_BUTTON_CLAIM);
    script_control(&ui, H2_FRUIT_SLICE_BUTTON_CLAIM);
    script_control(&ui, H2_FRUIT_SLICE_BUTTON_BACK);
    script_control(&ui, H2_FRUIT_SLICE_BUTTON_SHOP);
    for (unsigned weapon = 3u; weapon < H2_FRUIT_SLICE_WEAPONS; ++weapon) {
      script_visit(&ui, H2_FRUIT_SLICE_BUTTON_WEAPON + weapon);
      script_control(&ui, H2_FRUIT_SLICE_BUTTON_WEAPON + weapon);
    }
    /* An owned active weapon tap must not charge a second time. */
    script_control(&ui,
                   H2_FRUIT_SLICE_BUTTON_WEAPON + H2_FRUIT_SLICE_WEAPON_STORM);
    CHECK(run() == H2_PAL_OK);
    CHECK(fx.touch.script_next == fx.touch.script_count &&
          fx.audio_opens == 0u); /* Both mute bits survived. */
    h2_sqlite_destroy(storage);
    CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
    restored = sqlite_progress(storage, H2_FRUIT_SLICE_PROGRESS_BYTES);
    CHECK(restored.coins == 265u && restored.fruit_credit == 10u &&
          restored.combo_credit == 3u);
    CHECK(restored.score_task == variant && restored.equipped == 5u &&
          restored.owned == (((1u << (variant + 1u)) - 1u) | 56u));
    CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                           H2_FRUIT_SLICE_PREF_NAMESPACE,
                           H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
    uint32_t value = 0u;
    CHECK(ns->get_u32(ns, "best", &value) == H2_PAL_OK && value == 141u);
    CHECK(ns->get_u32(ns, "audio_v1", &value) == H2_PAL_OK &&
          value == 0x41550103u);
    void* bytes = NULL;
    size_t size = 0u;
    CHECK(ns->get_blob(ns, h2_desktop_platform_default_allocator(), "cards_v1",
                       &bytes, &size) == H2_PAL_OK);
    CHECK(size == sizeof(old_cards) && memcmp(bytes, old_cards, size) == 0);
    h2_pal_mem_free(h2_desktop_platform_default_allocator(), bytes);
    CHECK(ns->close(ns) == H2_PAL_OK);
    h2_sqlite_destroy(storage);
    CHECK(remove(database) == 0);
  }
}

static struct {
  const h2_pal_pref_api_t* real;
  h2_pal_pref_namespace_set_blob_fn set_blob;
  h2_pal_pref_namespace_commit_fn commit;
  bool writing_progress;
  unsigned write_failures, commit_failures, writes;
} profile_fault;

static int profile_set(h2_pal_pref_namespace_t* ns, const char* key,
                       const void* data, size_t size) {
  profile_fault.writing_progress = strcmp(key, "progress_v1") == 0;
  if (profile_fault.writing_progress) {
    ++profile_fault.writes;
    if (profile_fault.write_failures) {
      --profile_fault.write_failures;
      return H2_PAL_ERR_IO;
    }
  }
  return profile_fault.set_blob(ns, key, data, size);
}
static int profile_commit(h2_pal_pref_namespace_t* ns) {
  if (profile_fault.writing_progress && profile_fault.commit_failures) {
    --profile_fault.commit_failures;
    return H2_PAL_ERR_IO;
  }
  return profile_fault.commit(ns);
}
static int profile_open(void* user, const char* name,
                        h2_pal_pref_open_mode_t mode,
                        h2_pal_pref_namespace_t** ns) {
  (void)user;
  const int rc = h2_pal_pref_open(profile_fault.real, name, mode, ns);
  if (rc == H2_PAL_OK) {
    profile_fault.writing_progress = false;
    profile_fault.set_blob = (*ns)->set_blob;
    profile_fault.commit = (*ns)->commit;
    (*ns)->set_blob = profile_set;
    (*ns)->commit = profile_commit;
  }
  return rc;
}
static const h2_pal_pref_vtable_t profile_fault_vtable = {.open = profile_open};
static const h2_pal_pref_api_t profile_fault_api = {.vtable =
                                                        &profile_fault_vtable};

static void test_challenge_claim_retry_sqlite(void) {
  const char* temporary = getenv("TEST_TMPDIR");
  CHECK(temporary != NULL);
  char database[512];
  (void)snprintf(database, sizeof(database), "%s/challenge-retry.sqlite",
                 temporary);
  h2_sqlite_t* storage = NULL;
  const h2_sqlite_config_t config = {.path = database};
  CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
  h2_fruit_slice_game_t ui;
  h2_fruit_slice_game_init(&ui, WIDTH, HEIGHT, 1u, 0u);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CHALLENGES; ++i)
    ui.progress.challenge_credit[i] = 1u;
  uint8_t bytes[H2_FRUIT_SLICE_PROGRESS_BYTES];
  h2_fruit_slice_progress_encode(&ui.progress, bytes);
  h2_pal_pref_namespace_t* ns = NULL;
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
  CHECK(ns->set_blob(ns, "progress_v1", bytes, sizeof(bytes)) == H2_PAL_OK);
  CHECK(ns->commit(ns) == H2_PAL_OK && ns->close(ns) == H2_PAL_OK);
  memset(&profile_fault, 0, sizeof(profile_fault));
  profile_fault.real = h2_sqlite_pref_api(storage);
  profile_fault.write_failures = profile_fault.commit_failures = 1u;
  reset(28u);
  fx.fast_clock = true;
  fx.touch_from_frame = 2u;
  fx.pref_override = &profile_fault_api;
  script_control(&ui, H2_FRUIT_SLICE_BUTTON_TASKS);
  for (unsigned id = 3u; id < H2_FRUIT_SLICE_TASKS; ++id) {
    script_visit(&ui, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    script_control(&ui, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    script_control(&ui, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
  }
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  CHECK(profile_fault.writes >= 4u && profile_fault.write_failures == 0u &&
        profile_fault.commit_failures == 0u);
  h2_sqlite_destroy(storage);
  CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
  h2_fruit_slice_progress_t saved =
      sqlite_progress(storage, H2_FRUIT_SLICE_PROGRESS_BYTES);
  CHECK(saved.coins == 105u);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CHALLENGES; ++i)
    CHECK(saved.challenge_credit[i] == 0u);
  reset(28u);
  fx.fast_clock = true;
  fx.touch_from_frame = 2u;
  fx.pref_override = h2_sqlite_pref_api(storage);
  h2_fruit_slice_game_init(&ui, WIDTH, HEIGHT, 1u, 0u);
  ui.progress = saved;
  script_control(&ui, H2_FRUIT_SLICE_BUTTON_TASKS);
  for (unsigned id = 3u; id < H2_FRUIT_SLICE_TASKS; ++id) {
    script_visit(&ui, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
    script_control(&ui, H2_FRUIT_SLICE_BUTTON_CLAIM + id);
  }
  CHECK(run() == H2_PAL_OK);
  CHECK(sqlite_progress(storage, H2_FRUIT_SLICE_PROGRESS_BYTES).coins == 105u);

  /* A failed debit save and a failed retry commit must not debit again. */
  memset(&profile_fault, 0, sizeof(profile_fault));
  profile_fault.real = h2_sqlite_pref_api(storage);
  profile_fault.write_failures = profile_fault.commit_failures = 1u;
  reset(90u);
  fx.fast_clock = true;
  fx.touch_from_frame = 2u;
  fx.pref_override = &profile_fault_api;
  h2_fruit_slice_game_init(&ui, WIDTH, HEIGHT, 1u, 0u);
  ui.progress = saved;
  script_control(&ui, H2_FRUIT_SLICE_BUTTON_SHOP);
  script_visit(&ui, H2_FRUIT_SLICE_BUTTON_WEAPON + 3u);
  script_control(&ui, H2_FRUIT_SLICE_BUTTON_WEAPON + 3u);
  script_control(&ui, H2_FRUIT_SLICE_BUTTON_WEAPON + 3u);
  script_control(&ui,
                 H2_FRUIT_SLICE_BUTTON_WEAPON + 4u); /* 50 is not enough. */
  CHECK(run() == H2_PAL_OK);
  CHECK(profile_fault.writes >= 3u && profile_fault.write_failures == 0u &&
        profile_fault.commit_failures == 0u);
  h2_sqlite_destroy(storage);
  CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
  saved = sqlite_progress(storage, H2_FRUIT_SLICE_PROGRESS_BYTES);
  CHECK(saved.coins == 50u && saved.owned == 9u && saved.equipped == 3u);
  h2_sqlite_destroy(storage);
  CHECK(remove(database) == 0);
}

static void prepare_natural_result_profile(void) {
  /* With nine fruit kinds, seed 20 preserves the natural 12-point/two-card
   * result in both language routes without changing the award assertions. */
  fx.seed = 20u;
  memset(&pref, 0, sizeof(pref));
  pref.best_value = 1u;
  const h2_fruit_slice_progress_t initial = {.owned = 1u};
  h2_fruit_slice_progress_encode(&initial, pref.saved);
  pref.exists = pref.cards_enabled = true;
  fx.use_pref = true;
}

static void check_natural_result_awards(void) {
  h2_fruit_slice_progress_t progress;
  CHECK(h2_fruit_slice_progress_decode(&progress, pref.saved,
                                       sizeof(pref.saved)));
  CHECK(progress.fruit_credit == 12u && progress.combo_credit == 0u);
  h2_fruit_slice_cards_t cards;
  h2_fruit_slice_cards_init(&cards, 160, 160, 42u);
  CHECK(h2_fruit_slice_cards_decode(&cards, pref.cards_saved,
                                    sizeof(pref.cards_saved)));
  CHECK(h2_fruit_slice_cards_owned_total(&cards) == 2u);
}

static void test_natural_result_text(void) {
  reset(620u);
  prepare_natural_result_profile();
  fx.display.info.width = fx.display.info.height = 160;
  fx.touch.info.width = fx.touch.info.height = 160;
  fx.fast_clock = true;
  fx.autoplay_until = 340u;
  fx.touch_from_frame = 600u;
  fx.capture_name = "actual-result-default-zh-160x160";
  CHECK(run() == H2_PAL_OK);
  check_natural_result_awards();
  const int sizes[][2] = {{160, 160}, {240, 240}, {480, 320}, {800, 480}};
  for (unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
    for (unsigned language = 0; language < 2u; ++language) {
      reset(620u);
      prepare_natural_result_profile();
      fx.display.info.width = fx.touch.info.width = sizes[size][0];
      fx.display.info.height = fx.touch.info.height = sizes[size][1];
      fx.fast_clock = true;
      fx.autoplay_until = 340u;
      fx.autoplay_from = 6u;
      fx.reveal_until = 600u;
      fx.touch_from_frame = 2u;
      fx.language =
          language ? H2_FRUIT_SLICE_LANGUAGE_ZH_CN : H2_FRUIT_SLICE_LANGUAGE_EN;
      script_tap_at(2u, 30, 16); /* Both real language-switch directions. */
      char name[96];
      (void)snprintf(name, sizeof(name), "actual-result-toggle-%s-%dx%d",
                     language ? "en" : "zh", sizes[size][0], sizes[size][1]);
      fx.capture_name = name;
      CHECK(run() == H2_PAL_OK);
      CHECK(fx.ready_calls == 1u);
      check_natural_result_awards();
    }
  }
}

static void test_layout_app_routes(int32_t width, int32_t height,
                                   h2_fruit_slice_language_t language) {
  reset(500u);
  fx.fast_clock = true;
  fx.display.info.width = fx.touch.info.width = width;
  fx.display.info.height = fx.touch.info.height = height;
  fx.touch_from_frame = 2u;
  fx.autoplay_from = 40u;
  fx.seed = 111u;
  fx.language = language;
  fx.layout_sweep = true;
  fx.autoplay_until = 300u;
  char name[64];
  (void)snprintf(name, sizeof(name), "actual-app-layout-%s-%dx%d",
                 language == H2_FRUIT_SLICE_LANGUAGE_EN ? "en" : "zh", width,
                 height);
  fx.capture_name = name;
  const unsigned captures[] = {5u, 15u, 25u, 30u, 270u, 300u, 400u, 430u};
  memcpy(fx.capture_frames, captures, sizeof(captures));
  fx.capture_count = sizeof(captures) / sizeof(captures[0]);
  h2_fruit_slice_game_t layout;
  h2_fruit_slice_game_init(&layout, width, height, 42u, 0u);
  const uint32_t pages[] = {H2_FRUIT_SLICE_BUTTON_TASKS,
                            H2_FRUIT_SLICE_BUTTON_CARDS,
                            H2_FRUIT_SLICE_BUTTON_SHOP};
  for (unsigned i = 0; i < 3u; ++i) {
    h2_fruit_slice_rect_t r;
    CHECK(h2_fruit_slice_game_button_rect(&layout, pages[i], &r));
    script_tap_at(2u + i * 10u, (int32_t)((r.x0 + r.x1) * 0.5f),
                  (int32_t)((r.y0 + r.y1) * 0.5f));
    layout.phase = H2_FRUIT_SLICE_PHASE_TASKS;
    CHECK(h2_fruit_slice_game_button_rect(&layout, H2_FRUIT_SLICE_BUTTON_BACK,
                                          &r));
    script_tap_at(8u + i * 10u, (int32_t)((r.x0 + r.x1) * 0.5f),
                  (int32_t)((r.y0 + r.y1) * 0.5f));
    layout.phase = H2_FRUIT_SLICE_PHASE_TITLE;
  }
  h2_fruit_slice_rect_t start;
  CHECK(h2_fruit_slice_game_button_rect(&layout, H2_FRUIT_SLICE_BUTTON_START,
                                        &start));
  script_tap_at(32u, (int32_t)((start.x0 + start.x1) * 0.5f),
                (int32_t)((start.y0 + start.y1) * 0.5f));
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  CHECK(fx.display.presents > 100u && fx.ready_calls == 1u);
}

static void test_layout_loaded_profile(int32_t width, int32_t height,
                                       h2_fruit_slice_language_t language) {
  memset(&pref, 0, sizeof(pref));
  pref.best_value = UINT32_MAX;
  const h2_fruit_slice_progress_t progress = {
      .coins = H2_FRUIT_SLICE_PROGRESS_LIMIT,
      .fruit_credit = 10u,
      .combo_credit = 3u,
      .score_task = 1u,
      .owned = 7u,
      .equipped = 2u};
  h2_fruit_slice_progress_encode(&progress, pref.saved);
  pref.exists = true;
  h2_fruit_slice_cards_t stored;
  h2_fruit_slice_cards_init(&stored, width, height, 42u);
  for (unsigned id = 0; id < H2_FRUIT_SLICE_CARD_COUNT; ++id)
    stored.counts[id] = 99u;
  h2_fruit_slice_cards_encode(&stored, pref.cards_saved);
  pref.cards_enabled = pref.cards_exists = true;
  reset(240u);
  fx.use_pref = fx.fast_clock = true;
  fx.language = language;
  fx.display.info.width = fx.touch.info.width = width;
  fx.display.info.height = fx.touch.info.height = height;
  fx.touch_from_frame = 2u;
  char name[80];
  (void)snprintf(name, sizeof(name), "actual-app-profile-%s-%dx%d",
                 language == H2_FRUIT_SLICE_LANGUAGE_EN ? "en" : "zh", width,
                 height);
  fx.capture_name = name;
  h2_fruit_slice_game_t layout;
  h2_fruit_slice_game_init(&layout, width, height, 42u, UINT32_MAX);
  h2_fruit_slice_rect_t r;
  CHECK(h2_fruit_slice_game_button_rect(&layout, H2_FRUIT_SLICE_BUTTON_LANGUAGE,
                                        &r));
  const int32_t lx = (int32_t)((r.x0 + r.x1) * 0.5f);
  const int32_t ly = (int32_t)((r.y0 + r.y1) * 0.5f);
  script_tap_at(2u, lx, ly);
  fx.capture_frames[fx.capture_count++] = 5u;
  script_tap_at(8u, lx, ly);
  fx.capture_frames[fx.capture_count++] = 11u;
  CHECK(h2_fruit_slice_game_button_rect(&layout, H2_FRUIT_SLICE_BUTTON_TASKS,
                                        &r));
  script_tap_at(14u, (int32_t)((r.x0 + r.x1) * 0.5f),
                (int32_t)((r.y0 + r.y1) * 0.5f));
  fx.capture_frames[fx.capture_count++] = 17u;
  layout.phase = H2_FRUIT_SLICE_PHASE_TASKS;
  CHECK(
      h2_fruit_slice_game_button_rect(&layout, H2_FRUIT_SLICE_BUTTON_BACK, &r));
  const int32_t bx = (int32_t)((r.x0 + r.x1) * 0.5f);
  const int32_t by = (int32_t)((r.y0 + r.y1) * 0.5f);
  script_tap_at(20u, bx, by);
  layout.phase = H2_FRUIT_SLICE_PHASE_TITLE;
  CHECK(
      h2_fruit_slice_game_button_rect(&layout, H2_FRUIT_SLICE_BUTTON_SHOP, &r));
  script_tap_at(24u, (int32_t)((r.x0 + r.x1) * 0.5f),
                (int32_t)((r.y0 + r.y1) * 0.5f));
  fx.capture_frames[fx.capture_count++] = 27u;
  script_tap_at(30u, bx, by);
  CHECK(h2_fruit_slice_game_button_rect(&layout, H2_FRUIT_SLICE_BUTTON_CARDS,
                                        &r));
  script_tap_at(34u, (int32_t)((r.x0 + r.x1) * 0.5f),
                (int32_t)((r.y0 + r.y1) * 0.5f));
  h2_fruit_slice_album_layout_t album;
  h2_fruit_slice_cards_album_layout(width, height, layout.unit, &album);
  unsigned frame = 36u;
  for (unsigned tab = 0; tab < H2_FRUIT_SLICE_CARD_TABS; ++tab) {
    const h2_fruit_slice_card_rect_t* t = &album.tabs[tab];
    script_tap_at(frame, (int32_t)((t->x0 + t->x1) * 0.5f),
                  (int32_t)((t->y0 + t->y1) * 0.5f));
    frame += 2u;
    uint8_t ids[H2_FRUIT_SLICE_CARD_TAB_SLOTS];
    const unsigned count = h2_fruit_slice_card_tab_cards(tab, ids);
    for (unsigned slot = 0; slot < count; ++slot) {
      const h2_fruit_slice_card_rect_t* s = &album.slots[slot];
      script_tap_at(frame, (int32_t)((s->x0 + s->x1) * 0.5f),
                    (int32_t)((s->y0 + s->y1) * 0.5f));
      fx.capture_frames[fx.capture_count++] = frame + 7u;
      frame += 8u;
    }
  }
  script_tap_at(frame, bx, by);
  fx.exit_after = frame + 12u;
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  h2_fruit_slice_progress_t after;
  CHECK(h2_fruit_slice_progress_decode(&after, pref.saved, sizeof(pref.saved)));
  CHECK(after.coins == progress.coins && after.owned == 7u &&
        after.equipped == 2u);
}

static void test_layout_claim_buy_and_back(int32_t width, int32_t height,
                                           h2_fruit_slice_language_t language) {
  memset(&pref, 0, sizeof(pref));
  const h2_fruit_slice_progress_t initial = {.score_task = 1u, .owned = 1u};
  h2_fruit_slice_progress_encode(&initial, pref.saved);
  pref.exists = true;
  reset(50u);
  fx.use_pref = fx.fast_clock = true;
  fx.language = language;
  fx.display.info.width = fx.touch.info.width = width;
  fx.display.info.height = fx.touch.info.height = height;
  fx.touch_from_frame = 2u;
  char name[80];
  (void)snprintf(name, sizeof(name), "actual-app-economy-%s-%dx%d",
                 language == H2_FRUIT_SLICE_LANGUAGE_EN ? "en" : "zh", width,
                 height);
  fx.capture_name = name;
  h2_fruit_slice_game_t g;
  h2_fruit_slice_game_init(&g, width, height, 42u, 21u);
  g.progress = initial;
  g.language = language;
  script_control(&g, H2_FRUIT_SLICE_BUTTON_TASKS);
  script_visit(&g, H2_FRUIT_SLICE_BUTTON_CLAIM + 2u);
  fx.header_from = 2u + (unsigned)fx.touch.script_count;
  script_control(&g, H2_FRUIT_SLICE_BUTTON_CLAIM + 2u);
  fx.header_check = fx.claim_check = 2u + (unsigned)fx.touch.script_count;
  fx.capture_frames[fx.capture_count++] = fx.header_check;
  script_control(&g, H2_FRUIT_SLICE_BUTTON_BACK);
  script_control(&g, H2_FRUIT_SLICE_BUTTON_TASKS);
  if (width == 160 && height == 160) {
    script_tap(115, 124); /* Old conflict point is Back, not a claim. */
    h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_DOWN, 115, 124);
    h2_fruit_slice_game_pointer(&g, H2_FRUIT_SLICE_POINTER_UP, 115, 124);
  } else
    script_control(&g, H2_FRUIT_SLICE_BUTTON_BACK);
  script_control(&g, H2_FRUIT_SLICE_BUTTON_SHOP);
  script_visit(&g, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  script_control(&g, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  fx.capture_frames[fx.capture_count++] = 2u + (unsigned)fx.touch.script_count;
  script_visit(&g, H2_FRUIT_SLICE_BUTTON_WEAPON);
  script_control(&g, H2_FRUIT_SLICE_BUTTON_WEAPON);
  script_visit(&g, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  script_control(&g, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
  script_control(&g, H2_FRUIT_SLICE_BUTTON_BACK);
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.touch.script_next == fx.touch.script_count);
  h2_fruit_slice_progress_t saved;
  CHECK(h2_fruit_slice_progress_decode(&saved, pref.saved, sizeof(pref.saved)));
  CHECK(saved.coins == 0u && saved.owned == 3u && saved.equipped == 1u &&
        saved.score_task == 2u);
}

/* A real SQLite namespace remains owned by its writer while the public App
 * continues accepting controls. Later run-loop iterations release the
 * barrier; static menus need not present when their pixels are unchanged. */
typedef struct held_namespace {
  h2_pal_pref_namespace_t base;
  h2_pal_pref_namespace_t* real;
  bool held;
} held_namespace_t;

static struct {
  const h2_pal_pref_api_t* real;
  h2_pal_queue_t* entered;
  h2_pal_queue_t* release;
  h2_fruit_slice_game_t model;
  h2_pal_task_t* save_task;
  const char* held_key;
  bool block, held, observed, appended, released;
  bool fail_commit, fail_close, fail_start, fail_join;
  unsigned starts, failed_joins, held_frame;
  size_t held_presents;
  uint64_t started_ms;
} background;

static int held_get_blob(h2_pal_pref_namespace_t* base,
                         const h2_pal_mem_api_t* allocator, const char* key,
                         void** out, size_t* size) {
  held_namespace_t* ns = (held_namespace_t*)base;
  return ns->real->get_blob(ns->real, allocator, key, out, size);
}

static int held_get_u32(h2_pal_pref_namespace_t* base, const char* key,
                        uint32_t* out) {
  held_namespace_t* ns = (held_namespace_t*)base;
  return ns->real->get_u32(ns->real, key, out);
}

static int held_set_u32(h2_pal_pref_namespace_t* base, const char* key,
                        uint32_t value) {
  held_namespace_t* ns = (held_namespace_t*)base;
  return ns->real->set_u32(ns->real, key, value);
}

static int held_set_blob(h2_pal_pref_namespace_t* base, const char* key,
                         const void* data, size_t size) {
  held_namespace_t* ns = (held_namespace_t*)base;
  if (background.block && !background.held &&
      !strcmp(key, background.held_key)) {
    background.held = ns->held = true;
    const unsigned token = 1u;
    unsigned reply = 0u;
    const h2_pal_queue_api_t* queues = h2_desktop_platform_queue_api();
    CHECK(h2_pal_queue_send(queues, background.entered, &token, 0u) ==
          H2_PAL_OK);
    CHECK(h2_pal_queue_recv(queues, background.release, &reply, 5000u) ==
          H2_PAL_OK);
  }
  return ns->real->set_blob(ns->real, key, data, size);
}

static int held_commit(h2_pal_pref_namespace_t* base) {
  held_namespace_t* ns = (held_namespace_t*)base;
  if (ns->held && background.fail_commit) {
    background.fail_commit = false;
    return H2_PAL_ERR_IO;
  }
  return ns->real->commit(ns->real);
}

static int held_close(h2_pal_pref_namespace_t* base) {
  held_namespace_t* ns = (held_namespace_t*)base;
  int rc = ns->real->close(ns->real);
  if (ns->held && background.fail_close) {
    background.fail_close = false;
    rc = H2_PAL_ERR_IO;
  }
  free(ns);
  return rc;
}

static int held_open(void* user, const char* name, h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)user;
  *out = NULL;
  held_namespace_t* ns = calloc(1u, sizeof(*ns));
  CHECK(ns != NULL);
  const int rc = h2_pal_pref_open(background.real, name, mode, &ns->real);
  if (rc != H2_PAL_OK) {
    free(ns);
    return rc;
  }
  ns->base = (h2_pal_pref_namespace_t){.get_blob = held_get_blob,
                                       .get_u32 = held_get_u32,
                                       .set_u32 = held_set_u32,
                                       .set_blob = held_set_blob,
                                       .commit = held_commit,
                                       .close = held_close};
  *out = &ns->base;
  return H2_PAL_OK;
}

static int background_task_start(void* user,
                                 const h2_pal_task_options_t* options,
                                 h2_pal_task_entry_t entry, void* ctx,
                                 h2_pal_task_t** out) {
  (void)user;
  const bool save = !strcmp(options->name, "fruit-slice/save");
  if (save) {
    ++background.starts;
    if (background.fail_start) {
      background.fail_start = false;
      *out = NULL;
      return H2_PAL_ERR_NO_MEMORY;
    }
  }
  const int rc = h2_pal_task_start(h2_desktop_platform_task_api(), options,
                                   entry, ctx, out);
  if (save && rc == H2_PAL_OK) background.save_task = *out;
  return rc;
}

static int background_task_join(void* user, h2_pal_task_t* task) {
  (void)user;
  if (task == background.save_task && background.fail_join &&
      fx.frames > fx.exit_after) {
    background.fail_join = false;
    ++background.failed_joins;
    return H2_PAL_ERR_TASK;
  }
  return h2_pal_task_join(h2_desktop_platform_task_api(), task);
}

static const h2_pal_pref_vtable_t held_pref_vtable = {.open = held_open};
static const h2_pal_pref_api_t held_preferences = {.vtable = &held_pref_vtable};
static const h2_pal_task_vtable_t background_task_vtable = {
    .start = background_task_start, .join = background_task_join};
static const h2_pal_task_api_t background_tasks = {.vtable =
                                                       &background_task_vtable};

static void script_select_card(unsigned slot) {
  h2_fruit_slice_album_layout_t layout;
  h2_fruit_slice_game_t* game = &background.model;
  h2_fruit_slice_cards_album_layout(game->width, game->height, game->unit,
                                    &layout);
  const h2_fruit_slice_card_rect_t* r = &layout.slots[slot];
  const int x = (int)((r->x0 + r->x1) * 0.5f);
  const int y = (int)((r->y0 + r->y1) * 0.5f);
  script_tap(x, y);
  h2_fruit_slice_game_pointer(game, H2_FRUIT_SLICE_POINTER_DOWN, x, y);
  h2_fruit_slice_game_pointer(game, H2_FRUIT_SLICE_POINTER_UP, x, y);
}

static void background_loop(void) {
  uint64_t now = 0u;
  CHECK(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &now) ==
        H2_PAL_OK);
  CHECK(now - background.started_ms < 5000u);
  const h2_pal_queue_api_t* queues = h2_desktop_platform_queue_api();
  if (!background.observed) {
    unsigned token = 0u;
    if (h2_pal_queue_recv(queues, background.entered, &token, 0u) ==
        H2_PAL_OK) {
      background.observed = true;
      background.held_frame = fx.frames;
      background.held_presents = fx.display.presents;
    }
  }
  if (!background.appended && (!background.block || background.observed)) {
    h2_fruit_slice_game_t* game = &background.model;
    script_control(game, H2_FRUIT_SLICE_BUTTON_BACK);
    if (!strcmp(background.held_key, "cards_v1")) {
      script_control(game, H2_FRUIT_SLICE_BUTTON_TASKS);
      script_visit(game, H2_FRUIT_SLICE_BUTTON_CLAIM + 2u);
      script_control(game, H2_FRUIT_SLICE_BUTTON_CLAIM + 2u);
      script_control(game, H2_FRUIT_SLICE_BUTTON_BACK);
    }
    script_control(game, H2_FRUIT_SLICE_BUTTON_SHOP);
    script_visit(game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
    script_control(game, H2_FRUIT_SLICE_BUTTON_WEAPON + 1u);
    script_control(game, H2_FRUIT_SLICE_BUTTON_BACK);
    script_control(game, H2_FRUIT_SLICE_BUTTON_CARDS);
    script_select_card(0u);
    script_select_card(1u);
    script_control(game, H2_FRUIT_SLICE_BUTTON_BACK);
    background.appended = true;
  }
  if (background.appended && !background.released &&
      fx.touch.script_next == fx.touch.script_count) {
    if (background.block) {
      CHECK(fx.frames > background.held_frame + 4u);
      CHECK(fx.display.presents > background.held_presents);
      const unsigned token = 1u;
      CHECK(h2_pal_queue_send(queues, background.release, &token, 0u) ==
            H2_PAL_OK);
    }
    background.released = true;
    /* Exit immediately after the last input frame: its state must win even
     * when the older writer has not yet reported completion. */
    fx.exit_after = fx.frames;
  }
}

static void test_background_sqlite_save(unsigned failure, bool hold_cards) {
  const char* temporary = getenv("TEST_TMPDIR");
  CHECK(temporary != NULL);
  char database[512];
  const int n =
      snprintf(database, sizeof(database), "%s/background-%u-%u.sqlite",
               temporary, failure, (unsigned)hold_cards);
  CHECK(n > 0 && (size_t)n < sizeof(database));
  h2_sqlite_t* storage = NULL;
  const h2_sqlite_config_t config = {.path = database};
  CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
  h2_pal_pref_namespace_t* ns = NULL;
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_WRITE, &ns) == H2_PAL_OK);
  const h2_fruit_slice_progress_t initial = {.score_task = 1u, .owned = 1u};
  uint8_t progress[H2_FRUIT_SLICE_PROGRESS_BYTES];
  h2_fruit_slice_progress_encode(&initial, progress);
  CHECK(ns->set_blob(ns, "progress_v1", progress, sizeof(progress)) ==
        H2_PAL_OK);
  h2_fruit_slice_cards_t collection;
  h2_fruit_slice_cards_init(&collection, 800, 480, 42u);
  collection.counts[0] = 2u;
  collection.counts[1] = 3u;
  collection.unseen = 3u;
  uint8_t cards[H2_FRUIT_SLICE_CARDS_BYTES];
  h2_fruit_slice_cards_encode(&collection, cards);
  CHECK(ns->set_blob(ns, "cards_v1", cards, sizeof(cards)) == H2_PAL_OK);
  CHECK(ns->set_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, 266u) == H2_PAL_OK);
  CHECK(ns->set_u32(ns, "audio_v1", 2u) == H2_PAL_OK);
  CHECK(ns->commit(ns) == H2_PAL_OK && ns->close(ns) == H2_PAL_OK);

  memset(&background, 0, sizeof(background));
  background.real = h2_sqlite_pref_api(storage);
  background.held_key = hold_cards ? "cards_v1" : "progress_v1";
  background.block = failure != 3u;
  background.fail_commit = failure == 1u;
  background.fail_close = failure == 2u;
  background.fail_start = failure == 3u;
  background.fail_join = failure == 4u;
  h2_pal_queue_config_t barrier = {
      .name = "app-save-barrier",
      .item_size = sizeof(unsigned),
      .item_count = 1u,
      .allocator = h2_desktop_platform_default_allocator()};
  CHECK(h2_pal_queue_create(h2_desktop_platform_queue_api(), &barrier,
                            &background.entered) == H2_PAL_OK);
  CHECK(h2_pal_queue_create(h2_desktop_platform_queue_api(), &barrier,
                            &background.release) == H2_PAL_OK);
  reset(10000u);
  fx.display.info.width = fx.touch.info.width = 800;
  fx.display.info.height = fx.touch.info.height = 480;
  fx.background_saves = true;
  fx.pref_override = &held_preferences;
  fx.task_override = &background_tasks;
  fx.loop_hook = background_loop;
  fx.touch_from_frame = 2u;
  h2_fruit_slice_game_init(&background.model, 800, 480, 42u, 266u);
  background.model.progress = initial;
  background.model.cards = collection;
  if (hold_cards) {
    script_control(&background.model, H2_FRUIT_SLICE_BUTTON_CARDS);
    script_select_card(0u);
  } else {
    script_control(&background.model, H2_FRUIT_SLICE_BUTTON_TASKS);
    script_visit(&background.model, H2_FRUIT_SLICE_BUTTON_CLAIM + 2u);
    script_control(&background.model, H2_FRUIT_SLICE_BUTTON_CLAIM + 2u);
  }
  CHECK(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(),
                                     &background.started_ms) == H2_PAL_OK);
  CHECK(run() == (failure == 4u ? H2_PAL_ERR_TASK : H2_PAL_OK));
  CHECK(background.appended && background.released);
  CHECK(background.block ? background.observed : background.starts == 1u);
  CHECK(background.failed_joins == (failure == 4u ? 1u : 0u));
  h2_pal_queue_destroy(h2_desktop_platform_queue_api(), background.entered);
  h2_pal_queue_destroy(h2_desktop_platform_queue_api(), background.release);
  h2_sqlite_destroy(storage);
  CHECK(h2_sqlite_create(&config, &storage) == H2_PAL_OK);
  CHECK(h2_pal_pref_open(h2_sqlite_pref_api(storage),
                         H2_FRUIT_SLICE_PREF_NAMESPACE,
                         H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK);
  void* data = NULL;
  size_t size = 0u;
  const h2_pal_mem_api_t* allocator = h2_desktop_platform_default_allocator();
  CHECK(ns->get_blob(ns, allocator, "progress_v1", &data, &size) == H2_PAL_OK);
  h2_fruit_slice_progress_t saved;
  CHECK(h2_fruit_slice_progress_decode(&saved, data, size));
  h2_pal_mem_free(allocator, data);
  CHECK(saved.coins == 0u && saved.owned == 3u && saved.equipped == 1u &&
        saved.score_task == 2u);
  CHECK(ns->get_blob(ns, allocator, "cards_v1", &data, &size) == H2_PAL_OK);
  CHECK(h2_fruit_slice_cards_decode(&collection, data, size));
  h2_pal_mem_free(allocator, data);
  CHECK(collection.counts[0] == 2u && collection.counts[1] == 3u &&
        collection.unseen == 0u);
  uint32_t value = 0u;
  CHECK(ns->get_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, &value) == H2_PAL_OK &&
        value == 266u);
  CHECK(ns->get_u32(ns, "audio_v1", &value) == H2_PAL_OK && value == 2u);
  CHECK(ns->close(ns) == H2_PAL_OK);
  h2_sqlite_destroy(storage);
}

static void test_terminal_failure_markers(void) {
  reset(10u);
  fx.display.fail_from_frame = 5u;
  fx.display.draw_result = H2_PAL_ERR_TIMEOUT;
  CHECK(run() == H2_PAL_ERR_TIMEOUT);
  CHECK(fx.failure_count == 1u && fx.failure_before_close);
  CHECK(strstr(fx.failure_message, "op=display_draw rc=-6") != NULL);
  CHECK(strstr(fx.failure_message, "phase=0 frames=4") != NULL);
  CHECK(strstr(fx.failure_message, "size=480x480") != NULL);
  CHECK(fx.display.close_calls == 1 && fx.touch.close_calls == 1);

  reset(10u);
  fx.display.fail_from_frame = 5u;
  fx.display.present_result = H2_PAL_ERR_TIMEOUT;
  CHECK(run() == H2_PAL_ERR_TIMEOUT);
  CHECK(fx.failure_count == 1u && fx.failure_before_close);
  CHECK(strstr(fx.failure_message, "op=display_present rc=-6") != NULL);
  CHECK(strstr(fx.failure_message, "phase=0 frames=4") != NULL);
  CHECK(fx.display.close_calls == 1 && fx.touch.close_calls == 1);

  reset(40u);
  fx.touch.poll_error = H2_PAL_ERR_TIMEOUT;
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.failure_count == 0u && fx.display.presents == 40u);

  reset(20u);
  fx.paced_clock = true;
  fx.clock_error = H2_PAL_ERR_TIMEOUT;
  fx.clock_fail_after_ms = 100u;
  CHECK(run() == H2_PAL_ERR_TIMEOUT);
  CHECK(fx.failure_count == 1u && fx.failure_before_close);
  CHECK(strstr(fx.failure_message, "op=clock_wait rc=-6") != NULL);

  reset(10u);
  fx.ready_result = H2_PAL_ERR_TIMEOUT;
  CHECK(run() == H2_PAL_ERR_TIMEOUT);
  CHECK(fx.failure_count == 1u && fx.failure_before_close);
  CHECK(strstr(fx.failure_message, "op=ready rc=-6") != NULL);

  /* Normal empty Runtime waits return TIMEOUT; they are not run failures. */
  reset(8u);
  fx.paced_clock = true;
  CHECK(run() == H2_PAL_OK);
  CHECK(fx.clock_ms >= 200u && fx.failure_count == 0u);
}

int main(void) {
  test_terminal_failure_markers();
  test_background_sqlite_save(0u, false);
  test_background_sqlite_save(0u, true);
  for (unsigned failure = 1u; failure <= 4u; ++failure)
    test_background_sqlite_save(failure, false);
  test_paced_audio_through_public_entry(false);
  test_paced_audio_through_public_entry(true);
  test_bilingual_last_page_claim_and_purchase();
  test_020_sqlite_profile_upgrade();
  test_challenge_claim_retry_sqlite();
  test_variety_actual_frames();
  for (unsigned language = 0; language < 2u; ++language) {
    const h2_fruit_slice_language_t value = language == 0u
                                                ? H2_FRUIT_SLICE_LANGUAGE_ZH_CN
                                                : H2_FRUIT_SLICE_LANGUAGE_EN;
    test_layout_loaded_profile(160, 160, value);
    test_layout_loaded_profile(240, 240, value);
    test_layout_loaded_profile(480, 320, value);
    test_layout_loaded_profile(800, 480, value); /* BK7258 RGB panel. */
    test_layout_claim_buy_and_back(160, 160, value);
    test_layout_claim_buy_and_back(240, 240, value);
    test_layout_claim_buy_and_back(480, 320, value);
  }
  test_layout_app_routes(160, 160, H2_FRUIT_SLICE_LANGUAGE_ZH_CN);
  test_layout_app_routes(240, 240, H2_FRUIT_SLICE_LANGUAGE_ZH_CN);
  test_layout_app_routes(480, 320, H2_FRUIT_SLICE_LANGUAGE_ZH_CN);
  test_layout_app_routes(368, 448, H2_FRUIT_SLICE_LANGUAGE_ZH_CN);
  test_layout_app_routes(480, 800, H2_FRUIT_SLICE_LANGUAGE_ZH_CN);
  test_layout_app_routes(160, 160, H2_FRUIT_SLICE_LANGUAGE_EN);
  test_layout_app_routes(240, 240, H2_FRUIT_SLICE_LANGUAGE_EN);
  test_layout_app_routes(480, 320, H2_FRUIT_SLICE_LANGUAGE_EN);
  test_natural_result_text();
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
  test_consumed_device_release();
  test_interleaved_contacts();
  test_refused_move_with_two_contacts();
  test_interrupted_tap(true);
  test_interrupted_tap(false);
  test_persistent_touch_failure();
  test_ready_failure_stops();
  test_audio_gesture_pause_and_exit();
  test_audio_backpressure_keeps_stream_playing();
  test_audio_close_errors_release_runtime_wrapped_track();
  puts("fruit slice app tests passed");
  return 0;
}
