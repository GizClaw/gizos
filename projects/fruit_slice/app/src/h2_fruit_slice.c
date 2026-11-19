#include "h2_fruit_slice.h"

#include "h2_fruit_slice_audio.h"
#include "h2_fruit_slice_audio_worker.h"
#include "h2_fruit_slice_game.h"
#include "h2_fruit_slice_render.h"
#include "h2_fruit_slice_save.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "fruit-slice"
#define TOUCH_EVENTS_PER_FRAME 16u
#define TOUCH_FAILURE_LIMIT 30u
#define EVENTS_PER_DRAIN 64u
#define DT_MAX_MS 50u
#define STATS_PERIOD_MS 10000u
#define PROGRESS_RETRY_MS 2000u
#define PROGRESS_KEY H2_FRUIT_SLICE_PROGRESS_KEY
#define CARDS_KEY H2_FRUIT_SLICE_CARDS_KEY
#define INTERVAL_BUCKET_MS 5u
#define INTERVAL_BUCKETS 128u

/* Changed bands and, per band, the half-open column range that changed. */
typedef struct dirty {
  uint64_t bands;
  int16_t x0[H2_FRUIT_SLICE_MAX_BANDS], x1[H2_FRUIT_SLICE_MAX_BANDS];
} dirty_t;

typedef struct session {
  h2_runtime_t* runtime;
  h2_fruit_slice_config_t config;
  const h2_pal_mem_api_t* allocator;
  h2_display_info_t display;
  float touch_scale_x, touch_scale_y;
  uint16_t* frame;
  uint16_t* background;
  /* Background plus static overlays; the scene is composed on top of it. */
  uint16_t* static_layer;
  h2_fruit_slice_sprites_t* sprites;
  h2_fruit_slice_static_key_t static_key;
  dirty_t static_cover, previous;
  bool first_frame, failure_reported;
  unsigned touch_failures;
  uint32_t frames, frames_since_stats;
  uint64_t stats_started_ms;
  /* Per stats window: compose and push time, and pushed pixels. */
  uint64_t compose_ms, push_ms, pixels_pushed;
  /* Per stats window: longest frame of work, and frames over the period. */
  uint32_t max_frame_ms, slow_frames;
  uint32_t blast_frames, blast_max_ms;
  uint32_t intervals[INTERVAL_BUCKETS], interval_count, interval_max_ms;
  uint64_t last_frame_ms, save_ms;
  uint32_t save_max_ms;
  uint32_t phase_mask, reveal_frames, audio_max_ms, audio_open_max_ms;
  uint64_t progress_save_after_ms, cards_save_after_ms;
  h2_fruit_slice_save_worker_t save_worker;
  bool background_saves, best_pending;
  uint32_t pending_best, save_jobs, save_worker_max_ms;
  uint64_t save_join_after_ms;
  h2_fruit_slice_game_t game;
  h2_fruit_slice_audio_t audio;
  bool audio_unlocked, audio_paused;
  uint8_t event_payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
} session_t;

static void log_line(const session_t* s, h2_pal_log_level_t level,
                     const char* message) {
  (void)h2_pal_log_write(s->runtime->log, level, LOG_TAG, message);
}

/* Report the first terminal boundary once, without adding a clock read,
 * retry, recovery attempt or a different result/teardown path. */
static int failure_at(session_t* s, const char* operation, int rc) {
  if (rc != H2_PAL_OK && !s->failure_reported) {
    s->failure_reported = true;
    char message[224];
    (void)snprintf(
        message, sizeof(message),
        "H2_FRUIT_SLICE_FAILURE op=%s rc=%d phase=%u frames=%u "
        "reveal=%u game_ms=%llu size=%dx%d touch_failures=%u save_pending=%u",
        operation, rc, (unsigned)s->game.phase, (unsigned)s->frames,
        (unsigned)s->game.cards.reveal_active,
        (unsigned long long)s->game.time_ms, (int)s->display.width,
        (int)s->display.height, s->touch_failures,
        (unsigned)(s->save_worker.task != NULL));
    log_line(s, H2_PAL_LOG_ERROR, message);
  }
  return rc;
}

static uint32_t load_best(session_t* s) {
  h2_pal_pref_namespace_t* ns = NULL;
  uint32_t best = 0;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc == H2_PAL_OK && ns != NULL) {
    rc = ns->get_u32 != NULL
             ? ns->get_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, &best)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc != H2_PAL_OK) best = 0;
    if (ns->close != NULL) (void)ns->close(ns);
  }
  char message[80];
  (void)snprintf(message, sizeof(message),
                 "H2_FRUIT_SLICE_BEST_LOAD best=%u rc=%d", (unsigned)best, rc);
  log_line(s, H2_PAL_LOG_INFO, message);
  return best;
}

static void best_saved(session_t* s, uint32_t best, int rc) {
  char message[80];
  (void)snprintf(message, sizeof(message),
                 "H2_FRUIT_SLICE_BEST_SAVE best=%u rc=%d", (unsigned)best, rc);
  log_line(s, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN, message);
}

/* Optional persistence: a failure keeps the in-memory best only. */
static void save_best(session_t* s, uint32_t best) {
  best_saved(s, best, h2_fruit_slice_save_best(s->runtime, best));
}

static void load_progress(session_t* s) {
  h2_pal_pref_namespace_t* ns = NULL;
  void* data = NULL;
  size_t size = 0u;
  /* Open writable to initialize a fresh provider's database/namespace. This
   * does not replace an existing blob: missing data is distinguished from a
   * read failure below, so unreadable profiles still cannot be overwritten. */
  int rc = h2_pal_pref_open(s->runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns == NULL) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    rc = ns->get_blob != NULL
             ? ns->get_blob(ns, s->allocator, PROGRESS_KEY, &data, &size)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK &&
        !h2_fruit_slice_progress_decode(&s->game.progress, data, size))
      rc = H2_PAL_ERR_INVALID_ARG;
    if (ns->close != NULL) (void)ns->close(ns);
  }
  if (data != NULL) h2_pal_mem_free(s->allocator, data);
  if (rc == H2_PAL_ERR_UNSUPPORTED) {
    s->game.save_status = 1u;
  } else if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) {
    /* Never replace a profile we could not read, including future formats. */
    s->game.save_status = 3u;
  }
  char message[80];
  (void)snprintf(message, sizeof(message), "H2_FRUIT_SLICE_PROGRESS_LOAD rc=%d",
                 rc);
  log_line(s, H2_PAL_LOG_INFO, message);
}

static void progress_saved(session_t* s, const uint8_t* written, int rc) {
  s->game.save_status = rc == H2_PAL_OK                ? 0u
                        : rc == H2_PAL_ERR_UNSUPPORTED ? 1u
                                                       : 2u;
  if (rc == H2_PAL_OK) {
    uint8_t latest[H2_FRUIT_SLICE_PROGRESS_BYTES];
    h2_fruit_slice_progress_encode(&s->game.progress, latest);
    if (memcmp(latest, written, sizeof(latest)) == 0)
      s->game.progress_dirty = false;
  }
  char message[80];
  (void)snprintf(message, sizeof(message), "H2_FRUIT_SLICE_PROGRESS_SAVE rc=%d",
                 rc);
  log_line(s, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN, message);
}

/* A single blob keeps currency, consumed task credits and ownership together.
 * Retain dirty state after I/O failure and retry at a bounded interval. */
static void save_progress(session_t* s) {
  if (!s->game.progress_dirty || s->game.save_status == 1u ||
      s->game.save_status == 3u)
    return;
  uint8_t data[H2_FRUIT_SLICE_PROGRESS_BYTES];
  h2_fruit_slice_progress_encode(&s->game.progress, data);
  const int rc =
      h2_fruit_slice_save_blob(s->runtime, PROGRESS_KEY, data, sizeof(data));
  s->game.progress_urgent = false;
  progress_saved(s, data, rc);
}

static void load_cards(session_t* s) {
  h2_fruit_slice_cards_t* cards = &s->game.cards;
  h2_pal_pref_namespace_t* ns = NULL;
  void* data = NULL;
  size_t size = 0u;
  bool missing = false;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns == NULL) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    rc = ns->get_blob != NULL
             ? ns->get_blob(ns, s->allocator, CARDS_KEY, &data, &size)
             : H2_PAL_ERR_UNSUPPORTED;
    missing = rc == H2_PAL_ERR_NOT_FOUND;
    if (rc == H2_PAL_OK && !h2_fruit_slice_cards_merge(cards, data, size))
      rc = H2_PAL_ERR_INVALID_ARG;
    if (ns->close != NULL) (void)ns->close(ns);
  }
  if (data != NULL) h2_pal_mem_free(s->allocator, data);
  if (rc == H2_PAL_OK || missing) {
    cards->loaded = true;
    cards->seen_before_load = 0u;
    cards->save_status = 0u;
  } else {
    /* Never write without a known baseline. Operational errors can recover;
     * unsupported storage and invalid/future blobs stay protected. */
    cards->save_status = rc == H2_PAL_ERR_UNSUPPORTED   ? 1u
                         : rc == H2_PAL_ERR_INVALID_ARG ? 3u
                                                        : 2u;
  }
  ++cards->revision;
  char message[80];
  (void)snprintf(message, sizeof(message), "H2_FRUIT_SLICE_CARDS_LOAD rc=%d",
                 rc);
  log_line(s, H2_PAL_LOG_INFO, message);
}

static void cards_saved(session_t* s, const uint8_t* written, int rc) {
  h2_fruit_slice_cards_t* cards = &s->game.cards;
  const uint32_t status = rc == H2_PAL_OK                ? 0u
                          : rc == H2_PAL_ERR_UNSUPPORTED ? 1u
                                                         : 2u;
  if (cards->save_status != status) ++cards->revision;
  cards->save_status = status;
  if (rc == H2_PAL_OK) {
    uint8_t latest[H2_FRUIT_SLICE_CARDS_BYTES];
    h2_fruit_slice_cards_encode(cards, latest);
    if (memcmp(latest, written, sizeof(latest)) == 0) cards->dirty = false;
  }
  char message[80];
  (void)snprintf(message, sizeof(message), "H2_FRUIT_SLICE_CARDS_SAVE rc=%d",
                 rc);
  log_line(s, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN, message);
}

static void save_cards(session_t* s) {
  h2_fruit_slice_cards_t* cards = &s->game.cards;
  if (cards->save_status == 1u || cards->save_status == 3u) return;
  if (!cards->loaded && cards->save_status == 2u) load_cards(s);
  if (!cards->loaded || !cards->dirty) return;
  uint8_t data[H2_FRUIT_SLICE_CARDS_BYTES];
  h2_fruit_slice_cards_encode(cards, data);
  cards_saved(
      s, data,
      h2_fruit_slice_save_blob(s->runtime, CARDS_KEY, data, sizeof(data)));
}

/* Apply only completed snapshots. Later play can change the same blob while
 * its previous checkpoint is being written; that newer dirty state survives. */
static int collect_saves(session_t* s, uint64_t now, bool wait) {
  if (!wait && now < s->save_join_after_ms) return H2_PAL_OK;
  h2_fruit_slice_save_completion_t result;
  bool completed = false;
  const int rc = h2_fruit_slice_save_worker_poll(&s->save_worker, wait, &result,
                                                 &completed);
  if (rc != H2_PAL_OK) {
    s->save_join_after_ms = now + 500u;
    return rc;
  }
  if (!completed) return H2_PAL_OK;
  s->save_join_after_ms = 0u;
  if (result.work_ms > s->save_worker_max_ms)
    s->save_worker_max_ms = result.work_ms;
  if (result.snapshot.fields & H2_FRUIT_SLICE_SAVE_BEST) {
    best_saved(s, result.snapshot.best, result.best_result);
    if (s->pending_best == result.snapshot.best) s->best_pending = false;
  }
  if (result.snapshot.fields & H2_FRUIT_SLICE_SAVE_PROGRESS)
    progress_saved(s, result.snapshot.progress, result.progress_result);
  if (result.snapshot.fields & H2_FRUIT_SLICE_SAVE_CARDS)
    cards_saved(s, result.snapshot.cards, result.cards_result);
  return H2_PAL_OK;
}

static void synchronous_saves(session_t* s, uint64_t now) {
  if (s->best_pending) {
    save_best(s, s->pending_best);
    s->best_pending = false;
  }
  if (s->game.progress_dirty && s->game.save_status != 1u &&
      s->game.save_status != 3u &&
      (s->game.progress_urgent || now >= s->progress_save_after_ms)) {
    save_progress(s);
    s->progress_save_after_ms = now + PROGRESS_RETRY_MS;
  }
  if ((s->game.cards.dirty ||
       (!s->game.cards.loaded && s->game.cards.save_status == 2u)) &&
      now >= s->cards_save_after_ms) {
    save_cards(s);
    s->cards_save_after_ms = now + PROGRESS_RETRY_MS;
  }
}

static void service_saves(session_t* s, uint64_t now) {
  uint32_t best = 0u;
  if (h2_fruit_slice_game_take_best(&s->game, &best)) {
    s->pending_best = best;
    s->best_pending = true;
  }
  if (collect_saves(s, now, false) != H2_PAL_OK)
    log_line(s, H2_PAL_LOG_WARN, "H2_FRUIT_SLICE_SAVE_JOIN_RETRY");
  if (s->save_worker.task != NULL) return;
  if (!s->background_saves) {
    synchronous_saves(s, now);
    return;
  }

  h2_fruit_slice_save_snapshot_t snapshot = {0};
  if (s->best_pending) {
    snapshot.fields |= H2_FRUIT_SLICE_SAVE_BEST;
    snapshot.best = s->pending_best;
  }
  if (s->game.progress_dirty && s->game.save_status != 1u &&
      s->game.save_status != 3u &&
      (s->game.progress_urgent || now >= s->progress_save_after_ms)) {
    snapshot.fields |= H2_FRUIT_SLICE_SAVE_PROGRESS;
    h2_fruit_slice_progress_encode(&s->game.progress, snapshot.progress);
  }
  h2_fruit_slice_cards_t* cards = &s->game.cards;
  if ((cards->dirty || (!cards->loaded && cards->save_status == 2u)) &&
      cards->save_status != 1u && cards->save_status != 3u &&
      now >= s->cards_save_after_ms) {
    /* Recover an unread baseline with the existing merge/protection rules,
     * only while no writer is active. Ordinary loaded checkpoints do no
     * storage I/O on the run thread. */
    if (!cards->loaded) load_cards(s);
    if (cards->loaded && cards->dirty) {
      snapshot.fields |= H2_FRUIT_SLICE_SAVE_CARDS;
      h2_fruit_slice_cards_encode(cards, snapshot.cards);
    } else {
      s->cards_save_after_ms = now + PROGRESS_RETRY_MS;
    }
  }
  if (snapshot.fields == 0u) return;
  const int rc = h2_fruit_slice_save_worker_start(&s->save_worker, &snapshot);
  if (rc != H2_PAL_OK) {
    /* Optional scheduling must never discard a save or spin on task creation.
     * A host that cannot start this worker retains the synchronous behavior. */
    s->background_saves = false;
    log_line(s, H2_PAL_LOG_WARN, "H2_FRUIT_SLICE_SAVE_WORKER_FAILED");
    synchronous_saves(s, now);
    return;
  }
  ++s->save_jobs;
  if (snapshot.fields & H2_FRUIT_SLICE_SAVE_PROGRESS) {
    s->game.progress_urgent = false;
    s->progress_save_after_ms = now + PROGRESS_RETRY_MS;
  }
  if (snapshot.fields & H2_FRUIT_SLICE_SAVE_CARDS)
    s->cards_save_after_ms = now + PROGRESS_RETRY_MS;
}

static h2_pal_result_t drain_events(session_t* s) {
  for (unsigned i = 0; i < EVENTS_PER_DRAIN; ++i) {
    h2_runtime_event_t event = {.payload = s->event_payload,
                                .payload_capacity = sizeof(s->event_payload)};
    const h2_pal_result_t rc = h2_runtime_poll_event(s->runtime, &event);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      return H2_PAL_OK;
    if (rc != H2_PAL_OK) return failure_at(s, "poll_event", rc);
  }
  /* Bounded: the remainder is drained after the next frame's wait. */
  return H2_PAL_OK;
}

static h2_pal_result_t poll_touch(session_t* s) {
  for (unsigned i = 0; i < TOUCH_EVENTS_PER_FRAME; ++i) {
    h2_pal_touch_event_t event = {0};
    const h2_pal_result_t rc =
        h2_pal_touch_poll_event(s->runtime->touch, &event);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) return H2_PAL_OK;
    if (rc == H2_PAL_ERR_IO || rc == H2_PAL_ERR_TIMEOUT) {
      if (s->touch_failures++ == 0u) {
        char message[64];
        (void)snprintf(message, sizeof(message),
                       "H2_FRUIT_SLICE_TOUCH_RETRY rc=%d", rc);
        log_line(s, H2_PAL_LOG_WARN, message);
      }
      /* Abandon the stroke rather than slicing along a stale segment. */
      h2_fruit_slice_game_pointer(&s->game, H2_FRUIT_SLICE_POINTER_CANCEL, 0.0f,
                                  0.0f);
      return s->touch_failures >= TOUCH_FAILURE_LIMIT
                 ? failure_at(s, "touch_poll", rc)
                 : H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) return failure_at(s, "touch_poll", rc);
    if (s->touch_failures != 0u) {
      s->touch_failures = 0u;
      log_line(s, H2_PAL_LOG_INFO, "H2_FRUIT_SLICE_TOUCH_RECOVERED");
    }
    h2_fruit_slice_pointer_t kind;
    switch (event.kind) {
      case H2_PAL_TOUCH_EVENT_DOWN:
        s->audio_unlocked = true;
        kind = H2_FRUIT_SLICE_POINTER_DOWN;
        break;
      case H2_PAL_TOUCH_EVENT_MOVE:
        kind = H2_FRUIT_SLICE_POINTER_MOVE;
        break;
      case H2_PAL_TOUCH_EVENT_UP:
        kind = H2_FRUIT_SLICE_POINTER_UP;
        break;
      default:
        continue;
    }
    h2_fruit_slice_game_pointer(&s->game, kind,
                                (float)event.x * s->touch_scale_x,
                                (float)event.y * s->touch_scale_y);
  }
  return H2_PAL_OK;
}

static uint64_t clock_ms(const session_t* s) {
  uint64_t now = 0;
  (void)h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
  return now;
}

static h2_pal_result_t service_audio(session_t* s, uint64_t now) {
  if (!s->audio_unlocked || s->audio_paused || !s->audio.available)
    return H2_PAL_OK;
  const bool opening =
      s->audio.track == NULL && s->audio.worker == NULL && s->audio.mute != 3u;
  const uint32_t attempts = s->audio.writes + s->audio.blocked_writes;
  const int played = h2_fruit_slice_audio_pump(&s->audio, now);
  if (opening || attempts != s->audio.writes + s->audio.blocked_writes ||
      played != H2_PAL_OK) {
    const uint64_t ended = clock_ms(s);
    const uint32_t elapsed = ended >= now ? (uint32_t)(ended - now) : 0u;
    if (elapsed > s->audio_max_ms) s->audio_max_ms = elapsed;
    if (opening && elapsed > s->audio_open_max_ms)
      s->audio_open_max_ms = elapsed;
  }
  if (played == H2_PAL_OK) return H2_PAL_OK;
  char message[80];
  (void)snprintf(message, sizeof(message),
                 "H2_FRUIT_SLICE_AUDIO_DISABLED rc=%d", played);
  log_line(s, H2_PAL_LOG_WARN, message);
  const int rc = h2_fruit_slice_audio_suspend(&s->audio);
  s->audio.available = false;
  return failure_at(s, "audio_suspend", rc);
}

static void dirty_from_canvas(dirty_t* out,
                              const h2_fruit_slice_canvas_t* canvas) {
  out->bands = canvas->bands;
  memcpy(out->x0, canvas->band_x0, sizeof(out->x0));
  memcpy(out->x1, canvas->band_x1, sizeof(out->x1));
}

static void dirty_merge(dirty_t* into, const dirty_t* other) {
  for (int32_t band = 0; band < H2_FRUIT_SLICE_MAX_BANDS; ++band) {
    const uint64_t bit = (uint64_t)1 << band;
    if (!(other->bands & bit)) continue;
    if (!(into->bands & bit)) {
      into->bands |= bit;
      into->x0[band] = other->x0[band];
      into->x1[band] = other->x1[band];
      continue;
    }
    if (other->x0[band] < into->x0[band]) into->x0[band] = other->x0[band];
    if (other->x1[band] > into->x1[band]) into->x1[band] = other->x1[band];
  }
}

static void dirty_all(dirty_t* out, const session_t* s) {
  out->bands = h2_fruit_slice_all_bands(s->display.height);
  for (int32_t band = 0; band < H2_FRUIT_SLICE_MAX_BANDS; ++band) {
    out->x0[band] = 0;
    out->x1[band] = (int16_t)s->display.width;
  }
}

/* Copy every dirty column range from source to destination. */
static void copy_dirty(const session_t* s, uint16_t* destination,
                       const uint16_t* source, const dirty_t* dirty) {
  const int32_t width = s->display.width, height = s->display.height;
  for (int32_t band = 0; band < H2_FRUIT_SLICE_MAX_BANDS; ++band) {
    if (!(dirty->bands & ((uint64_t)1 << band))) continue;
    const int32_t y0 = band * H2_FRUIT_SLICE_BAND_ROWS;
    if (y0 >= height) break;
    int32_t y1 = y0 + H2_FRUIT_SLICE_BAND_ROWS;
    if (y1 > height) y1 = height;
    const int32_t x0 = dirty->x0[band], x1 = dirty->x1[band];
    for (int32_t y = y0; y < y1; ++y) {
      const size_t offset = (size_t)y * (size_t)width + (size_t)x0;
      memcpy(destination + offset, source + offset,
             (size_t)(x1 - x0) * sizeof(uint16_t));
    }
  }
}

/* Rebuild the static layer when its key changed, adding what changed. */
static void update_static_layer(session_t* s, dirty_t* changed) {
  h2_fruit_slice_static_key_t key;
  h2_fruit_slice_static_key(&s->game, &key);
  if (!s->first_frame && memcmp(&key, &s->static_key, sizeof(key)) == 0) return;
  h2_fruit_slice_canvas_t canvas = {.pixels = s->static_layer,
                                    .width = s->display.width,
                                    .height = s->display.height,
                                    .stride_pixels = (size_t)s->display.width,
                                    .measure = true,
                                    .sprites = s->sprites};
  h2_fruit_slice_render_static(&canvas, &s->game);
  dirty_t current;
  dirty_from_canvas(&current, &canvas);
  if (s->first_frame) {
    dirty_all(changed, s);
  } else {
    dirty_merge(changed, &current);
    dirty_merge(changed, &s->static_cover);
  }
  copy_dirty(s, s->static_layer, s->background, changed);
  canvas.measure = false;
  canvas.bands = 0;
  h2_fruit_slice_render_static(&canvas, &s->game);
  s->static_key = key;
  s->static_cover = current;
}

/* Merge only equal column spans. A union across all adjacent bands turns
 * scattered particles into a large transfer of otherwise unchanged pixels.
 * Columns are widened to even boundaries for panels that need them. */
static h2_pal_result_t push_dirty(session_t* s, const dirty_t* dirty) {
  const int32_t width = s->display.width, height = s->display.height;
  const int32_t band_count =
      (height + H2_FRUIT_SLICE_BAND_ROWS - 1) / H2_FRUIT_SLICE_BAND_ROWS;
  h2_pal_result_t rc = H2_PAL_OK;
  for (int32_t band = 0; band < band_count && rc == H2_PAL_OK;) {
    if (!(dirty->bands & ((uint64_t)1 << band))) {
      ++band;
      continue;
    }
    const int32_t start = band;
    const int32_t x0 = dirty->x0[band] & ~1;
    int32_t x1 = (dirty->x1[band] + 1) & ~1;
    if (x1 > width) x1 = width;
    ++band;
    while (band < band_count && (dirty->bands & ((uint64_t)1 << band))) {
      const int32_t next_x0 = dirty->x0[band] & ~1;
      int32_t next_x1 = (dirty->x1[band] + 1) & ~1;
      if (next_x1 > width) next_x1 = width;
      if (next_x0 != x0 || next_x1 != x1) break;
      ++band;
    }
    const int32_t y0 = start * H2_FRUIT_SLICE_BAND_ROWS;
    int32_t y1 = band * H2_FRUIT_SLICE_BAND_ROWS;
    if (y1 > height) y1 = height;
    const h2_display_rect_t rect = {x0, y0, x1 - x0, y1 - y0};
    rc = h2_pal_display_draw_bitmap(
        s->runtime->display, &rect,
        s->frame + (size_t)y0 * (size_t)width + (size_t)x0,
        (size_t)width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
    if (rc != H2_PAL_OK) (void)failure_at(s, "display_draw", rc);
    s->pixels_pushed += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
    /* A panel may synchronously wait for its preceding DMA here. Do not
     * postpone every PCM deadline until all dirty bands have been sent. */
    if (rc == H2_PAL_OK && s->audio_unlocked)
      rc = service_audio(s, clock_ms(s));
  }
  if (rc == H2_PAL_OK && dirty->bands != 0u) {
    rc = h2_pal_display_present(s->runtime->display);
    if (rc != H2_PAL_OK) (void)failure_at(s, "display_present", rc);
  }
  return rc;
}

static h2_pal_result_t render_frame(session_t* s) {
  const int32_t width = s->display.width, height = s->display.height;
  const uint64_t started = clock_ms(s);
  dirty_t dirty = {0};
  update_static_layer(s, &dirty);
  h2_fruit_slice_canvas_t canvas = {.pixels = s->frame,
                                    .width = width,
                                    .height = height,
                                    .stride_pixels = (size_t)width,
                                    .measure = true,
                                    .sprites = s->sprites};
  h2_fruit_slice_render_scene(&canvas, &s->game);
  dirty_t current;
  dirty_from_canvas(&current, &canvas);
  dirty_merge(&dirty, &current);
  dirty_merge(&dirty, &s->previous);
  copy_dirty(s, s->frame, s->static_layer, &dirty);
  canvas.measure = false;
  canvas.bands = 0;
  h2_fruit_slice_render_scene(&canvas, &s->game);
  const uint64_t composed = clock_ms(s);
  h2_pal_result_t rc = service_audio(s, composed);
  if (rc == H2_PAL_OK) rc = push_dirty(s, &dirty);
  const uint64_t pushed = clock_ms(s);
  s->compose_ms += composed - started;
  s->push_ms += pushed - composed;
  if (rc == H2_PAL_OK) {
    s->previous = current;
    s->first_frame = false;
  }
  return rc;
}

/* Wait at least once, until the frame deadline, draining Runtime events. */
static h2_pal_result_t wait_frame(session_t* s, uint64_t deadline_ms) {
  bool waited = false;
  for (;;) {
    uint64_t now = 0;
    h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
    if (rc != H2_PAL_OK) return failure_at(s, "clock_wait", rc);
    const uint64_t remaining = now < deadline_ms ? deadline_ms - now : 0u;
    if (remaining == 0u && waited) return H2_PAL_OK;
    rc = service_audio(s, now);
    if (rc != H2_PAL_OK) return rc;
    uint32_t wait_ms = remaining == 0u ? 1u : (uint32_t)remaining;
    if (s->audio_unlocked && s->audio.available && s->audio.worker == NULL &&
        !s->audio_paused && s->audio.mute != 3u && wait_ms > 10u)
      wait_ms = 10u;
    rc = h2_runtime_wait_notify(s->runtime, wait_ms);
    waited = true;
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT)
      return failure_at(s, "wait_notify", rc);
    const h2_pal_result_t drained = drain_events(s);
    if (drained != H2_PAL_OK) return drained;
    /* SDK tick-rounded waits may return before the millisecond deadline.
     * Recheck it, including after TIMEOUT, to avoid frame pacing jitter. */
  }
}

static uint32_t interval_percentile(const session_t* s, uint32_t percent) {
  const uint32_t rank = (s->interval_count * percent + 99u) / 100u;
  if (rank == 0u) return 0u;
  uint32_t count = 0u;
  for (unsigned i = 0; i + 1u < INTERVAL_BUCKETS; ++i) {
    count += s->intervals[i];
    if (count >= rank) return i * INTERVAL_BUCKET_MS;
  }
  return s->interval_max_ms;
}

static void report_stats(session_t* s, uint64_t now) {
  const uint64_t elapsed = now - s->stats_started_ms;
  if (elapsed < STATS_PERIOD_MS) return;
  const uint64_t tenths = (uint64_t)s->frames_since_stats * 10000u / elapsed;
  const uint64_t frames = s->frames_since_stats ? s->frames_since_stats : 1u;
  /* Averages per frame, in tenths of a millisecond and in rows. */
  const uint64_t compose = s->compose_ms * 10u / frames;
  const uint64_t push = s->push_ms * 10u / frames;
  char message[320];
  (void)snprintf(
      message, sizeof(message),
      "H2_FRUIT_SLICE_STATS fps=%u.%u frames=%u phase=%u score=%u "
      "best=%u sliced=%u missed=%u compose_ms=%u.%u push_ms=%u.%u kpx=%u "
      "max_ms=%u slow=%u blast_frames=%u blast_max_ms=%u",
      (unsigned)(tenths / 10u), (unsigned)(tenths % 10u), (unsigned)s->frames,
      (unsigned)s->game.phase, (unsigned)s->game.score, (unsigned)s->game.best,
      (unsigned)s->game.sliced_total, (unsigned)s->game.missed_total,
      (unsigned)(compose / 10u), (unsigned)(compose % 10u),
      (unsigned)(push / 10u), (unsigned)(push % 10u),
      (unsigned)(s->pixels_pushed / frames / 1000u), (unsigned)s->max_frame_ms,
      (unsigned)s->slow_frames, (unsigned)s->blast_frames,
      (unsigned)s->blast_max_ms);
  log_line(s, H2_PAL_LOG_INFO, message);
  (void)snprintf(
      message, sizeof(message),
      "H2_FRUIT_SLICE_TIMING interval_p95_le_ms=%u interval_p99_le_ms=%u "
      "interval_max_ms=%u save_ms=%u save_max_ms=%u phases=0x%x reveals=%u",
      (unsigned)interval_percentile(s, 95u),
      (unsigned)interval_percentile(s, 99u), (unsigned)s->interval_max_ms,
      (unsigned)s->save_ms, (unsigned)s->save_max_ms, (unsigned)s->phase_mask,
      (unsigned)s->reveal_frames);
  log_line(s, H2_PAL_LOG_INFO, message);
  if (s->config.background_saves) {
    (void)snprintf(message, sizeof(message),
                   "H2_FRUIT_SLICE_SAVE_TIMING worker=%u pending=%u jobs=%u "
                   "worker_max_ms=%u",
                   (unsigned)s->background_saves,
                   (unsigned)(s->save_worker.task != NULL),
                   (unsigned)s->save_jobs, (unsigned)s->save_worker_max_ms);
    log_line(s, H2_PAL_LOG_INFO, message);
    s->save_jobs = s->save_worker_max_ms = 0u;
  }
  (void)snprintf(message, sizeof(message),
                 "H2_FRUIT_SLICE_AUDIO_TIMING gap_ms=%u writes=%u blocked=%u "
                 "rebases=%u max_ms=%u open_max_ms=%u command_drops=%u "
                 "worker=%u gap_scope=%s",
                 (unsigned)s->audio.max_pump_gap_ms, (unsigned)s->audio.writes,
                 (unsigned)s->audio.blocked_writes, (unsigned)s->audio.rebases,
                 (unsigned)s->audio_max_ms, (unsigned)s->audio_open_max_ms,
                 (unsigned)s->audio.dropped_commands,
                 s->audio.worker != NULL ? 1u : 0u,
                 s->audio.worker != NULL ? "stream" : "window");
  log_line(s, H2_PAL_LOG_INFO, message);
  if (s->config.report_diagnostics != NULL)
    s->config.report_diagnostics(s->config.user);
  s->phase_mask = s->reveal_frames = 0u;
  s->audio_max_ms = s->audio_open_max_ms = 0u;
  memset(s->intervals, 0, sizeof(s->intervals));
  s->interval_count = s->interval_max_ms = 0u;
  s->save_ms = s->save_max_ms = 0u;
  s->audio.max_pump_gap_ms = s->audio.writes = 0u;
  s->audio.blocked_writes = s->audio.rebases = 0u;
  s->stats_started_ms = now;
  s->frames_since_stats = 0;
  s->compose_ms = s->push_ms = s->pixels_pushed = 0;
  s->max_frame_ms = s->slow_frames = 0;
  s->blast_frames = s->blast_max_ms = 0;
}

static h2_pal_result_t play(session_t* s) {
  const uint32_t period = s->config.frame_period_ms != 0u
                              ? s->config.frame_period_ms
                              : H2_FRUIT_SLICE_DEFAULT_FRAME_PERIOD_MS;
  uint64_t last = 0;
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &last);
  if (rc != H2_PAL_OK) return failure_at(s, "clock_start", rc);
  s->stats_started_ms = last;
  bool ready = false;
  for (;;) {
    if (s->config.should_exit != NULL && s->config.should_exit(s->config.user))
      return H2_PAL_OK;
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
    if (rc != H2_PAL_OK) return failure_at(s, "clock_frame", rc);
    const uint64_t frame_start = now;
    if (s->frames != 0u && !s->audio_paused) {
      const uint64_t gap =
          now >= s->last_frame_ms ? now - s->last_frame_ms : 0u;
      const uint32_t interval = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
      unsigned bucket =
          interval / INTERVAL_BUCKET_MS + (interval % INTERVAL_BUCKET_MS != 0u);
      if (bucket >= INTERVAL_BUCKETS) bucket = INTERVAL_BUCKETS - 1u;
      ++s->intervals[bucket];
      ++s->interval_count;
      if (interval > s->interval_max_ms) s->interval_max_ms = interval;
    }
    s->last_frame_ms = now;
    if (s->config.is_paused != NULL && s->config.is_paused(s->config.user)) {
      if (!s->audio_paused) {
        rc = h2_fruit_slice_audio_suspend(&s->audio);
        if (rc != H2_PAL_OK) return failure_at(s, "audio_pause", rc);
        h2_fruit_slice_game_pointer(&s->game, H2_FRUIT_SLICE_POINTER_CANCEL, 0,
                                    0);
        s->game.sound_count = 0;
        s->audio_paused = true;
      }
      last = now;
      rc = wait_frame(s, now + 50u);
      if (rc != H2_PAL_OK) return rc;
      continue;
    }
    if (s->audio_paused) {
      last = now;
      s->audio_paused = false;
    }
    uint64_t dt = now > last ? now - last : 0u;
    if (dt > DT_MAX_MS) dt = DT_MAX_MS;
    last = now;

    rc = poll_touch(s);
    if (rc != H2_PAL_OK) return rc;
    h2_fruit_slice_game_step(&s->game, (uint32_t)dt);
    s->phase_mask |= 1u << (unsigned)s->game.phase;
    if (h2_fruit_slice_cards_reveal_active(&s->game.cards)) ++s->reveal_frames;
    if (s->audio.mute != s->game.audio_mute || s->audio.pref_dirty) {
      const int saved =
          h2_fruit_slice_audio_set_mute(&s->audio, s->game.audio_mute);
      if (saved != H2_PAL_OK)
        log_line(s, H2_PAL_LOG_WARN, "H2_FRUIT_SLICE_AUDIO_PREF_FAILED");
    }
    h2_fruit_slice_sound_t sound;
    while (h2_fruit_slice_game_take_sound(&s->game, &sound))
      h2_fruit_slice_audio_emit(&s->audio, sound, now);
    rc = service_audio(s, clock_ms(s));
    if (rc != H2_PAL_OK) return rc;
    rc = render_frame(s);
    if (rc != H2_PAL_OK) return failure_at(s, "render", rc);
    ++s->frames;
    ++s->frames_since_stats;

    if (!ready) {
      ready = true;
      if (s->config.brightness_percent != 0u) {
        const uint32_t percent = s->config.brightness_percent > 100u
                                     ? 100u
                                     : s->config.brightness_percent;
        const int brightness =
            h2_pal_display_set_brightness_percent(s->runtime->display, percent);
        if (brightness != H2_PAL_OK) {
          char message[64];
          (void)snprintf(message, sizeof(message),
                         "H2_FRUIT_SLICE_BRIGHTNESS rc=%d", brightness);
          log_line(s, H2_PAL_LOG_WARN, message);
        }
      }
      char message[96];
      (void)snprintf(message, sizeof(message),
                     "H2_FRUIT_SLICE_READY width=%d height=%d",
                     s->display.width, s->display.height);
      log_line(s, H2_PAL_LOG_INFO, message);
      if (s->config.ready != NULL) {
        rc = s->config.ready(s->config.user);
        if (rc != H2_PAL_OK) return failure_at(s, "ready", rc);
      }
    }

    const uint64_t save_started = clock_ms(s);
    service_saves(s, now);
    const uint64_t saved = clock_ms(s);
    const uint64_t saving = saved >= save_started ? saved - save_started : 0u;
    s->save_ms += saving;
    if (saving > s->save_max_ms) s->save_max_ms = (uint32_t)saving;
    const uint64_t worked = saved > frame_start ? saved - frame_start : 0u;
    if (worked > s->max_frame_ms) s->max_frame_ms = (uint32_t)worked;
    if (worked > period) ++s->slow_frames;
    if (s->game.flash_ms != 0u) {
      ++s->blast_frames;
      if (worked > s->blast_max_ms) s->blast_max_ms = (uint32_t)worked;
    }
    report_stats(s, saved);

    rc = wait_frame(s, frame_start + period);
    if (rc != H2_PAL_OK) return rc;
  }
}

static h2_pal_result_t prepare(session_t* s) {
  h2_pal_result_t rc =
      h2_pal_display_get_info(s->runtime->display, &s->display);
  if (rc != H2_PAL_OK) return failure_at(s, "display_info", rc);
  if (s->display.native_format != H2_DISPLAY_PIXEL_RGB565 ||
      s->display.width < H2_FRUIT_SLICE_MIN_EXTENT ||
      s->display.width > H2_FRUIT_SLICE_MAX_EXTENT ||
      s->display.height < H2_FRUIT_SLICE_MIN_EXTENT ||
      s->display.height > H2_FRUIT_SLICE_MAX_EXTENT)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_touch_info_t touch = {0};
  rc = h2_pal_touch_get_info(s->runtime->touch, &touch);
  if (rc != H2_PAL_OK) return failure_at(s, "touch_info", rc);
  s->touch_scale_x =
      touch.width != 0u ? (float)s->display.width / (float)touch.width : 1.0f;
  s->touch_scale_y = touch.height != 0u
                         ? (float)s->display.height / (float)touch.height
                         : 1.0f;

  const size_t bytes =
      (size_t)s->display.width * (size_t)s->display.height * sizeof(uint16_t);
  s->frame = h2_pal_mem_alloc(s->allocator, bytes);
  s->background = h2_pal_mem_alloc(s->allocator, bytes);
  s->static_layer = h2_pal_mem_alloc(s->allocator, bytes);
  if (s->frame == NULL || s->background == NULL || s->static_layer == NULL) {
    char message[96];
    (void)snprintf(message, sizeof(message),
                   "H2_FRUIT_SLICE_ALLOC_FAIL what=frames bytes=%u",
                   (unsigned)bytes);
    log_line(s, H2_PAL_LOG_ERROR, message);
    return H2_PAL_ERR_NO_MEMORY;
  }
  s->sprites = h2_fruit_slice_sprites_create(s->allocator);
  if (s->sprites == NULL) {
    log_line(s, H2_PAL_LOG_ERROR, "H2_FRUIT_SLICE_ALLOC_FAIL what=sprites");
    return H2_PAL_ERR_NO_MEMORY;
  }
  h2_fruit_slice_canvas_t canvas = {.pixels = s->background,
                                    .width = s->display.width,
                                    .height = s->display.height,
                                    .stride_pixels = (size_t)s->display.width};
  h2_fruit_slice_render_background(&canvas);

  uint32_t seed = s->config.seed;
  if (seed == 0u) {
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
    if (rc != H2_PAL_OK) return failure_at(s, "clock_seed", rc);
    seed = (uint32_t)(now ^ (now >> 32)) * 2654435761u ^ 0xA511E9B3u;
    if (seed == 0u) seed = 1u;
  }
  h2_fruit_slice_game_init(&s->game, s->display.width, s->display.height, seed,
                           load_best(s));
  s->game.language = s->config.language == H2_FRUIT_SLICE_LANGUAGE_EN
                         ? H2_FRUIT_SLICE_LANGUAGE_EN
                         : H2_FRUIT_SLICE_LANGUAGE_ZH_CN;
  load_progress(s);
  load_cards(s);
  if (s->game.cards.save_status == 2u)
    s->cards_save_after_ms = clock_ms(s) + PROGRESS_RETRY_MS;
  if (s->config.background_saves) {
    const int saved =
        h2_fruit_slice_save_worker_init(&s->save_worker, s->runtime);
    s->background_saves = saved == H2_PAL_OK;
    if (!s->background_saves)
      log_line(s, H2_PAL_LOG_WARN, "H2_FRUIT_SLICE_SAVE_WORKER_UNAVAILABLE");
  }
  /* Build every fixed-size texture now so rendering never allocates. */
  if (!h2_fruit_slice_sprites_prepare(s->sprites, &s->game)) {
    log_line(s, H2_PAL_LOG_ERROR, "H2_FRUIT_SLICE_ALLOC_FAIL what=textures");
    return H2_PAL_ERR_NO_MEMORY;
  }
  const int audio_rc = h2_fruit_slice_audio_init(&s->audio, s->runtime);
  s->game.audio_mute = s->audio.mute;
  if (audio_rc != H2_PAL_OK) {
    char message[80];
    (void)snprintf(message, sizeof(message),
                   "H2_FRUIT_SLICE_AUDIO_UNAVAILABLE rc=%d", audio_rc);
    log_line(s, H2_PAL_LOG_WARN, message);
  }
  if (audio_rc == H2_PAL_OK && s->config.prewarm_audio) {
    const uint64_t started = clock_ms(s);
    const int prepared = h2_fruit_slice_audio_prepare(&s->audio);
    const uint64_t ended = clock_ms(s);
    char message[96];
    (void)snprintf(message, sizeof(message),
                   "H2_FRUIT_SLICE_AUDIO_PREPARE rc=%d ms=%u", prepared,
                   (unsigned)(ended >= started ? ended - started : 0u));
    log_line(s, prepared == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN,
             message);
    if (prepared != H2_PAL_OK) {
      rc = h2_fruit_slice_audio_suspend(&s->audio);
      if (rc != H2_PAL_OK) return failure_at(s, "audio_prepare_close", rc);
      s->audio.available = false;
    }
  }
  if (s->config.audio_worker && s->audio.available) {
    s->audio.worker_requested = true;
    if (s->audio.mute != 3u) {
      rc = h2_fruit_slice_audio_worker_start(&s->audio);
      if (rc != H2_PAL_OK) {
        log_line(s, H2_PAL_LOG_WARN, "H2_FRUIT_SLICE_AUDIO_WORKER_FAILED");
        const int stopped = h2_fruit_slice_audio_suspend(&s->audio);
        if (stopped != H2_PAL_OK)
          return failure_at(s, "audio_start_close", stopped);
        s->audio.available = false;
      }
    }
  }
  s->first_frame = true;
  return H2_PAL_OK;
}

h2_pal_result_t h2_fruit_slice_run(h2_runtime_t* runtime,
                                   const h2_fruit_slice_config_t* config) {
  if (runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t* allocator = runtime->mem;
  session_t* s = h2_pal_mem_alloc(allocator, sizeof(*s));
  if (s == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  s->runtime = runtime;
  s->allocator = allocator;
  if (config != NULL) s->config = *config;

  bool display_open = false, touch_open = false;
  h2_pal_result_t rc =
      failure_at(s, "display_open", h2_pal_display_open(runtime->display));
  if (rc == H2_PAL_OK) {
    display_open = true;
    rc = failure_at(s, "touch_open", h2_pal_touch_open(runtime->touch));
  }
  if (rc == H2_PAL_OK) {
    touch_open = true;
    rc = failure_at(s, "prepare", prepare(s));
  }
  if (rc == H2_PAL_OK) rc = failure_at(s, "play", play(s));
  /* The final write cannot race an older in-flight checkpoint. Retain the
   * Runtime and copied arguments through failed joins before flushing newer
   * state once, using the same optional-storage failure behavior as before. */
  int save_closed;
  while ((save_closed = collect_saves(s, clock_ms(s), true)) != H2_PAL_OK) {
    if (rc == H2_PAL_OK) rc = failure_at(s, "save_join", save_closed);
    log_line(s, H2_PAL_LOG_WARN, "H2_FRUIT_SLICE_SAVE_JOIN_RETRY");
    (void)h2_runtime_wait_notify(runtime, 50u);
  }
  if (s->best_pending) save_best(s, s->pending_best);
  save_progress(s);
  save_cards(s);
  h2_fruit_slice_save_worker_destroy(&s->save_worker);

  /* Audio borrows Runtime until track/speaker teardown completes. The final
   * UI intent may be newer than the last accepted playback queue command. */
  int audio_closed;
  while ((audio_closed = h2_fruit_slice_audio_close_with_mute(
              &s->audio, s->game.audio_mute)) != H2_PAL_OK) {
    if (rc == H2_PAL_OK) rc = failure_at(s, "audio_close", audio_closed);
    log_line(s, H2_PAL_LOG_WARN, "H2_FRUIT_SLICE_AUDIO_CLOSE_RETRY");
    (void)h2_runtime_wait_notify(runtime, 50u);
  }

  char message[64];
  (void)snprintf(message, sizeof(message),
                 "H2_FRUIT_SLICE_EXIT rc=%d frames=%u", rc,
                 (unsigned)s->frames);
  log_line(s, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR, message);

  if (touch_open) {
    const h2_pal_result_t closed = h2_pal_touch_close(runtime->touch);
    if (rc == H2_PAL_OK) rc = failure_at(s, "touch_close", closed);
  }
  if (display_open) {
    const int closed = h2_pal_display_close(runtime->display);
    if (rc == H2_PAL_OK) rc = failure_at(s, "display_close", closed);
  }
  h2_fruit_slice_sprites_destroy(s->sprites);
  h2_pal_mem_free(allocator, s->static_layer);
  h2_pal_mem_free(allocator, s->background);
  h2_pal_mem_free(allocator, s->frame);
  h2_pal_mem_free(allocator, s);
  return rc;
}
