#include "h2_garden_guard.h"

#include "h2_garden_guard_game.h"
#include "h2_garden_guard_render.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "garden-guard"
#define TOUCH_EVENTS_PER_FRAME 16u
#define TOUCH_FAILURE_LIMIT 30u
#define EVENTS_PER_DRAIN 64u
#define DT_MAX_MS 100u
#define STATS_PERIOD_MS 10000u
#define PROFILE_KEY "profile"
#define LANGUAGE_KEY "language"
#define PROFILE_RETRY_MS 2000u
#define PART_COUNT ((unsigned)H2_GARDEN_GUARD_PART_COUNT)

/* Changed bands and, per band, the half-open column range that changed. */
typedef struct dirty {
  uint64_t bands;
  int16_t x0[H2_GARDEN_GUARD_MAX_BANDS], x1[H2_GARDEN_GUARD_MAX_BANDS];
} dirty_t;

typedef struct session {
  h2_runtime_t* runtime;
  h2_garden_guard_config_t config;
  const h2_pal_mem_api_t* allocator;
  h2_display_info_t display;
  float touch_scale_x, touch_scale_y;
  uint16_t* frame;
  uint16_t* background;
  /* Background plus the static parts; the scene is composed on top of it. */
  uint16_t* static_layer;
  /* Nearest static figure per pixel, for occlusion. */
  uint8_t* depth;
  h2_garden_guard_static_key_t static_key[PART_COUNT];
  dirty_t static_cover[PART_COUNT];
  dirty_t previous;
  uint64_t idle_pose_tick, profile_retry_ms;
  uint32_t plant_revision;
  bool previous_transient;
  bool first_frame;
  unsigned touch_failures;
  uint32_t frames, frames_since_stats;
  uint64_t stats_started_ms;
  /* Per stats window: compose and push time, and pushed pixels. */
  uint64_t compose_ms, push_ms, pixels_pushed;
  /* Per stats window: longest frame of work, and frames over the period. */
  uint32_t max_frame_ms, slow_frames;
  h2_garden_guard_game_t game;
  uint8_t event_payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
} session_t;

static void log_line(const session_t* s, h2_pal_log_level_t level,
                     const char* message) {
  (void)h2_pal_log_write(s->runtime->log, level, LOG_TAG, message);
}

static uint32_t load_best(session_t* s) {
  h2_pal_pref_namespace_t* ns = NULL;
  uint32_t best = 0;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_GARDEN_GUARD_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc == H2_PAL_OK && ns != NULL) {
    rc = ns->get_u32 != NULL
             ? ns->get_u32(ns, H2_GARDEN_GUARD_PREF_BEST_KEY, &best)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc != H2_PAL_OK) best = 0;
    if (ns->close != NULL) (void)ns->close(ns);
  }
  char message[80];
  (void)snprintf(message, sizeof(message),
                 "H2_GARDEN_GUARD_BEST_LOAD best=%u rc=%d", (unsigned)best, rc);
  log_line(s, H2_PAL_LOG_INFO, message);
  return best;
}

/* Optional persistence: a failure keeps the in-memory best only. */
static void save_best(session_t* s, uint32_t best) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_GARDEN_GUARD_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns != NULL) {
    rc = ns->set_u32 != NULL
             ? ns->set_u32(ns, H2_GARDEN_GUARD_PREF_BEST_KEY, best)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK && ns->commit != NULL) rc = ns->commit(ns);
    if (ns->close != NULL) {
      const int closed = ns->close(ns);
      if (rc == H2_PAL_OK) rc = closed;
    }
  }
  char message[80];
  (void)snprintf(message, sizeof(message),
                 "H2_GARDEN_GUARD_BEST_SAVE best=%u rc=%d", (unsigned)best, rc);
  log_line(s, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN, message);
}

static void load_profile(session_t* s) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_GARDEN_GUARD_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc == H2_PAL_ERR_NOT_FOUND) return;
  if (rc == H2_PAL_OK && ns != NULL) {
    void* data = NULL;
    size_t size = 0;
    rc = ns->get_blob == NULL
             ? H2_PAL_ERR_UNSUPPORTED
             : ns->get_blob(ns, s->allocator, PROFILE_KEY, &data, &size);
    if (rc == H2_PAL_OK) {
      h2_garden_guard_profile_t profile;
      if (h2_garden_guard_profile_decode(&profile, data, size)) {
        if (profile.best < s->game.best) profile.best = s->game.best;
        s->game.profile = profile;
        s->game.best = profile.best;
      } else {
        /* Leave an unrecognized record available to a newer app. */
        rc = H2_PAL_ERR_INVALID_ARG;
      }
    }
    if (data != NULL) h2_pal_mem_free(s->allocator, data);
    if (ns->close != NULL) (void)ns->close(ns);
  }
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) {
    s->game.save_state = H2_GARDEN_GUARD_SAVE_SESSION;
    log_line(s, H2_PAL_LOG_WARN, "H2_GARDEN_GUARD_PROFILE_SESSION_ONLY");
  }
}

static void save_profile(session_t* s, uint64_t now) {
  if (!s->game.profile_dirty ||
      s->game.save_state == H2_GARDEN_GUARD_SAVE_SESSION ||
      now < s->profile_retry_ms)
    return;
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_GARDEN_GUARD_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns != NULL) {
    uint8_t bytes[H2_GARDEN_GUARD_SAVE_BYTES];
    h2_garden_guard_profile_encode(&s->game.profile, bytes);
    rc = ns->set_blob == NULL
             ? H2_PAL_ERR_UNSUPPORTED
             : ns->set_blob(ns, PROFILE_KEY, bytes, sizeof(bytes));
    if (rc == H2_PAL_OK && ns->commit != NULL) rc = ns->commit(ns);
    if (ns->close != NULL) (void)ns->close(ns);
  } else if (rc == H2_PAL_OK) {
    rc = H2_PAL_ERR_UNSUPPORTED;
  }
  if (rc == H2_PAL_OK) {
    s->game.profile_dirty = false;
    s->game.save_state = H2_GARDEN_GUARD_SAVE_READY;
    char message[112];
    (void)snprintf(
        message, sizeof(message),
        "H2_GARDEN_GUARD_PROFILE_SAVED seen=%u owned=%u coins=%u total=%u",
        (unsigned)s->game.profile.seen, (unsigned)s->game.profile.owned,
        (unsigned)s->game.profile.coins, (unsigned)s->game.profile.total_score);
    log_line(s, H2_PAL_LOG_INFO, message);
  } else {
    const uint8_t state = rc == H2_PAL_ERR_UNSUPPORTED
                              ? H2_GARDEN_GUARD_SAVE_SESSION
                              : H2_GARDEN_GUARD_SAVE_RETRY;
    if (s->game.save_state != state)
      log_line(s, H2_PAL_LOG_WARN, "H2_GARDEN_GUARD_PROFILE_SAVE_FAILED");
    s->game.save_state = state;
    s->profile_retry_ms = now + PROFILE_RETRY_MS;
  }
}

static void load_language(session_t* s) {
  h2_pal_pref_namespace_t* ns = NULL;
  uint32_t language = 0;
  if (h2_pal_pref_open(s->runtime->pref, H2_GARDEN_GUARD_PREF_NAMESPACE,
                       H2_PAL_PREF_OPEN_READ_ONLY, &ns) == H2_PAL_OK &&
      ns != NULL) {
    if (ns->get_u32 != NULL &&
        ns->get_u32(ns, LANGUAGE_KEY, &language) == H2_PAL_OK &&
        language == H2_GARDEN_GUARD_LANG_ZH)
      s->game.language = H2_GARDEN_GUARD_LANG_ZH;
    if (ns->close != NULL) (void)ns->close(ns);
  }
}

static void notify_language(const session_t* s) {
  if (s->config.language_changed != NULL)
    s->config.language_changed(
        s->config.user,
        s->game.language == H2_GARDEN_GUARD_LANG_ZH ? "zh-CN" : "en");
}

static void save_language(session_t* s) {
  if (!s->game.language_dirty) return;
  s->game.language_dirty = false;
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_GARDEN_GUARD_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns != NULL) {
    rc = ns->set_u32 != NULL ? ns->set_u32(ns, LANGUAGE_KEY, s->game.language)
                             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK && ns->commit != NULL) rc = ns->commit(ns);
    if (ns->close != NULL) (void)ns->close(ns);
  } else if (rc == H2_PAL_OK) {
    rc = H2_PAL_ERR_UNSUPPORTED;
  }
  s->game.language_unsaved = rc != H2_PAL_OK;
  notify_language(s);
}

static h2_pal_result_t drain_events(session_t* s) {
  for (unsigned i = 0; i < EVENTS_PER_DRAIN; ++i) {
    h2_runtime_event_t event = {.payload = s->event_payload,
                                .payload_capacity = sizeof(s->event_payload)};
    const h2_pal_result_t rc = h2_runtime_poll_event(s->runtime, &event);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      return H2_PAL_OK;
    if (rc != H2_PAL_OK) return rc;
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
                       "H2_GARDEN_GUARD_TOUCH_RETRY rc=%d", rc);
        log_line(s, H2_PAL_LOG_WARN, message);
      }
      /* Abandon the gesture rather than finish it at a stale point. */
      h2_garden_guard_game_pointer(&s->game, H2_GARDEN_GUARD_POINTER_CANCEL,
                                   0.0f, 0.0f);
      return s->touch_failures >= TOUCH_FAILURE_LIMIT ? rc : H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) return rc;
    if (s->touch_failures != 0u) {
      s->touch_failures = 0u;
      log_line(s, H2_PAL_LOG_INFO, "H2_GARDEN_GUARD_TOUCH_RECOVERED");
    }
    h2_garden_guard_pointer_t kind;
    switch (event.kind) {
      case H2_PAL_TOUCH_EVENT_DOWN:
        kind = H2_GARDEN_GUARD_POINTER_DOWN;
        break;
      case H2_PAL_TOUCH_EVENT_MOVE:
        kind = H2_GARDEN_GUARD_POINTER_MOVE;
        break;
      case H2_PAL_TOUCH_EVENT_UP:
        kind = H2_GARDEN_GUARD_POINTER_UP;
        break;
      default:
        continue;
    }
    h2_garden_guard_game_pointer(&s->game, kind,
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

static void dirty_from_canvas(dirty_t* out,
                              const h2_garden_guard_canvas_t* canvas) {
  out->bands = canvas->bands;
  memcpy(out->x0, canvas->band_x0, sizeof(out->x0));
  memcpy(out->x1, canvas->band_x1, sizeof(out->x1));
}

static void dirty_merge(dirty_t* into, const dirty_t* other) {
  for (int32_t band = 0; band < H2_GARDEN_GUARD_MAX_BANDS; ++band) {
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
  out->bands = h2_garden_guard_all_bands(s->display.height);
  for (int32_t band = 0; band < H2_GARDEN_GUARD_MAX_BANDS; ++band) {
    out->x0[band] = 0;
    out->x1[band] = (int16_t)s->display.width;
  }
}

/* Forget depth ownership wherever static pixels are restored. */
static void clear_depth(const session_t* s, const dirty_t* dirty) {
  const int32_t width = s->display.width, height = s->display.height;
  for (int32_t band = 0; band < H2_GARDEN_GUARD_MAX_BANDS; ++band) {
    if (!(dirty->bands & ((uint64_t)1 << band))) continue;
    const int32_t y0 = band * H2_GARDEN_GUARD_BAND_ROWS;
    if (y0 >= height) break;
    int32_t y1 = y0 + H2_GARDEN_GUARD_BAND_ROWS;
    if (y1 > height) y1 = height;
    const int32_t x0 = dirty->x0[band], x1 = dirty->x1[band];
    for (int32_t y = y0; y < y1; ++y)
      memset(s->depth + (size_t)y * (size_t)width + (size_t)x0, 0,
             (size_t)(x1 - x0));
  }
}

/* Copy every dirty column range from source to destination. */
static void copy_dirty(const session_t* s, uint16_t* destination,
                       const uint16_t* source, const dirty_t* dirty) {
  const int32_t width = s->display.width, height = s->display.height;
  for (int32_t band = 0; band < H2_GARDEN_GUARD_MAX_BANDS; ++band) {
    if (!(dirty->bands & ((uint64_t)1 << band))) continue;
    const int32_t y0 = band * H2_GARDEN_GUARD_BAND_ROWS;
    if (y0 >= height) break;
    int32_t y1 = y0 + H2_GARDEN_GUARD_BAND_ROWS;
    if (y1 > height) y1 = height;
    const int32_t x0 = dirty->x0[band], x1 = dirty->x1[band];
    for (int32_t y = y0; y < y1; ++y) {
      const size_t offset = (size_t)y * (size_t)width + (size_t)x0;
      memcpy(destination + offset, source + offset,
             (size_t)(x1 - x0) * sizeof(uint16_t));
    }
  }
}

static h2_garden_guard_canvas_t layer_canvas(const session_t* s,
                                             uint16_t* pixels, bool measure) {
  return (h2_garden_guard_canvas_t){.pixels = pixels,
                                    .width = s->display.width,
                                    .height = s->display.height,
                                    .stride_pixels = (size_t)s->display.width,
                                    .measure = measure,
                                    .depth = s->depth};
}

/* Rebuild the static parts whose key changed, adding what changed. A part
 * that shares a band with the restored area is repainted whole after its own
 * cover is restored too, so no pixel is blended twice. */
static void update_static_layer(session_t* s, dirty_t* changed) {
  h2_garden_guard_static_key_t keys[PART_COUNT];
  dirty_t covers[PART_COUNT];
  bool repaint[PART_COUNT];
  for (unsigned p = 0; p < PART_COUNT; ++p) {
    const h2_garden_guard_part_t part = (h2_garden_guard_part_t)p;
    h2_garden_guard_static_key(&s->game, part, &keys[p]);
    repaint[p] = s->first_frame ||
                 memcmp(&keys[p], &s->static_key[p], sizeof(keys[p])) != 0;
    if (!repaint[p]) {
      covers[p] = s->static_cover[p];
      continue;
    }
    h2_garden_guard_canvas_t canvas = layer_canvas(s, s->static_layer, true);
    h2_garden_guard_render_static(&canvas, &s->game, part);
    dirty_from_canvas(&covers[p], &canvas);
    dirty_merge(changed, &covers[p]);
    dirty_merge(changed, &s->static_cover[p]);
  }
  if (s->first_frame) dirty_all(changed, s);
  if (changed->bands == 0u) return;
  for (bool grew = true; grew;) {
    grew = false;
    for (unsigned p = 0; p < PART_COUNT; ++p) {
      if (repaint[p] || !(covers[p].bands & changed->bands)) continue;
      repaint[p] = true;
      dirty_merge(changed, &covers[p]);
      grew = true;
    }
  }
  copy_dirty(s, s->static_layer, s->background, changed);
  clear_depth(s, changed);
  for (unsigned p = 0; p < PART_COUNT; ++p) {
    if (!repaint[p]) continue;
    h2_garden_guard_canvas_t canvas = layer_canvas(s, s->static_layer, false);
    h2_garden_guard_render_static(&canvas, &s->game, (h2_garden_guard_part_t)p);
    s->static_key[p] = keys[p];
    s->static_cover[p] = covers[p];
  }
}

/* Merge only equal column spans so scattered sprites do not turn into a
 * large transfer of unchanged pixels. Columns widen to even boundaries. */
static h2_pal_result_t push_dirty(session_t* s, const dirty_t* dirty) {
  const int32_t width = s->display.width, height = s->display.height;
  const int32_t band_count =
      (height + H2_GARDEN_GUARD_BAND_ROWS - 1) / H2_GARDEN_GUARD_BAND_ROWS;
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
    const int32_t y0 = start * H2_GARDEN_GUARD_BAND_ROWS;
    int32_t y1 = band * H2_GARDEN_GUARD_BAND_ROWS;
    if (y1 > height) y1 = height;
    const h2_display_rect_t rect = {x0, y0, x1 - x0, y1 - y0};
    rc = h2_pal_display_draw_bitmap(
        s->runtime->display, &rect,
        s->frame + (size_t)y0 * (size_t)width + (size_t)x0,
        (size_t)width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
    s->pixels_pushed += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
  }
  if (rc == H2_PAL_OK && dirty->bands != 0u)
    rc = h2_pal_display_present(s->runtime->display);
  return rc;
}

/* Idle foliage has a slower pose clock. Between its ticks, leave the
 * displayed pixels alone unless the current or previous scene has effects
 * that can move at full speed. This includes the last frame of an exit. */
static bool scene_has_transient(const h2_garden_guard_game_t* game) {
  if (game->menu == H2_GARDEN_GUARD_MENU_JOURNAL)
    return h2_garden_guard_journal_animating(game);
  if (h2_garden_guard_paused(game)) return false;
  if (game->banner_ms != 0u || game->hover_lane >= 0) return true;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PARTICLES; ++i)
    if (game->particles[i].active) return true;
  if (game->phase != H2_GARDEN_GUARD_PHASE_PLAYING) return false;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_SUNS; ++i)
    if (game->suns[i].active) return true;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PEAS; ++i)
    if (game->peas[i].active) return true;
  for (unsigned i = 0; i < H2_GARDEN_GUARD_MAX_PESTS; ++i)
    if (game->pests[i].active || game->pests[i].exit_ms != 0u) return true;
  for (unsigned lane = 0; lane < H2_GARDEN_GUARD_LANES; ++lane) {
    if (game->carts[lane].state == H2_GARDEN_GUARD_CART_SWEEPING) return true;
    for (unsigned col = 0; col < H2_GARDEN_GUARD_COLS; ++col) {
      const h2_garden_guard_plant_t* plant = &game->plants[lane][col];
      if (plant->grow_ms || plant->action_ms || plant->impact_ms) return true;
    }
  }
  return false;
}

static h2_pal_result_t render_frame(session_t* s) {
  const uint64_t started = clock_ms(s);
  dirty_t dirty = {0};
  update_static_layer(s, &dirty);
  const bool transient = scene_has_transient(&s->game);
  const uint64_t tick = s->game.phase == H2_GARDEN_GUARD_PHASE_PLAYING
                            ? s->game.time_ms / H2_GARDEN_GUARD_PLANT_IDLE_MS
                            : 0u;
  if (dirty.bands == 0u && !transient && !s->previous_transient &&
      tick == s->idle_pose_tick && s->plant_revision == s->game.plant_revision)
    return H2_PAL_OK;
  h2_garden_guard_canvas_t canvas = layer_canvas(s, s->frame, true);
  h2_garden_guard_render_scene(&canvas, &s->game);
  dirty_t current;
  dirty_from_canvas(&current, &canvas);
  dirty_merge(&dirty, &current);
  dirty_merge(&dirty, &s->previous);
  copy_dirty(s, s->frame, s->static_layer, &dirty);
  canvas = layer_canvas(s, s->frame, false);
  h2_garden_guard_render_scene(&canvas, &s->game);
  const uint64_t composed = clock_ms(s);
  const h2_pal_result_t rc = push_dirty(s, &dirty);
  const uint64_t pushed = clock_ms(s);
  s->compose_ms += composed - started;
  s->push_ms += pushed - composed;
  if (rc == H2_PAL_OK) {
    s->previous = current;
    s->previous_transient = transient;
    s->idle_pose_tick = tick;
    s->plant_revision = s->game.plant_revision;
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
    if (rc != H2_PAL_OK) return rc;
    const uint64_t remaining = now < deadline_ms ? deadline_ms - now : 0u;
    if (remaining == 0u && waited) return H2_PAL_OK;
    rc = h2_runtime_wait_notify(s->runtime,
                                remaining == 0u ? 1u : (uint32_t)remaining);
    waited = true;
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT) return rc;
    const h2_pal_result_t drained = drain_events(s);
    if (drained != H2_PAL_OK) return drained;
    if (rc == H2_PAL_ERR_TIMEOUT) return H2_PAL_OK;
  }
}

static void report_stats(session_t* s, uint64_t now) {
  const uint64_t elapsed = now - s->stats_started_ms;
  if (elapsed < STATS_PERIOD_MS) return;
  const uint64_t tenths = (uint64_t)s->frames_since_stats * 10000u / elapsed;
  const uint64_t frames = s->frames_since_stats ? s->frames_since_stats : 1u;
  /* Averages per frame, in tenths of a millisecond and in pixels. */
  const uint64_t compose = s->compose_ms * 10u / frames;
  const uint64_t push = s->push_ms * 10u / frames;
  char message[384];
  (void)snprintf(
      message, sizeof(message),
      "H2_GARDEN_GUARD_STATS fps=%u.%u frames=%u phase=%u wave=%u sun=%u "
      "score=%u best=%u kills=%u compose_ms=%u.%u push_ms=%u.%u kpx=%u "
      "max_ms=%u slow=%u",
      (unsigned)(tenths / 10u), (unsigned)(tenths % 10u), (unsigned)s->frames,
      (unsigned)s->game.phase, (unsigned)s->game.wave, (unsigned)s->game.sun,
      (unsigned)s->game.score, (unsigned)s->game.best, (unsigned)s->game.kills,
      (unsigned)(compose / 10u), (unsigned)(compose % 10u),
      (unsigned)(push / 10u), (unsigned)(push % 10u),
      (unsigned)(s->pixels_pushed / frames / 1000u), (unsigned)s->max_frame_ms,
      (unsigned)s->slow_frames);
  log_line(s, H2_PAL_LOG_INFO, message);
  s->stats_started_ms = now;
  s->frames_since_stats = 0;
  s->compose_ms = s->push_ms = s->pixels_pushed = 0;
  s->max_frame_ms = s->slow_frames = 0;
}

static h2_pal_result_t on_first_frame(session_t* s) {
  if (s->config.brightness_percent != 0u) {
    const uint32_t percent = s->config.brightness_percent > 100u
                                 ? 100u
                                 : s->config.brightness_percent;
    const int brightness =
        h2_pal_display_set_brightness_percent(s->runtime->display, percent);
    if (brightness != H2_PAL_OK) {
      char message[64];
      (void)snprintf(message, sizeof(message),
                     "H2_GARDEN_GUARD_BRIGHTNESS rc=%d", brightness);
      log_line(s, H2_PAL_LOG_WARN, message);
    }
  }
  char message[96];
  (void)snprintf(message, sizeof(message),
                 "H2_GARDEN_GUARD_READY width=%d height=%d portrait=%u",
                 s->display.width, s->display.height,
                 s->game.layout.portrait ? 1u : 0u);
  log_line(s, H2_PAL_LOG_INFO, message);
  return s->config.ready != NULL ? s->config.ready(s->config.user) : H2_PAL_OK;
}

static h2_pal_result_t play(session_t* s) {
  const uint32_t period = s->config.frame_period_ms != 0u
                              ? s->config.frame_period_ms
                              : H2_GARDEN_GUARD_DEFAULT_FRAME_PERIOD_MS;
  uint64_t last = 0;
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &last);
  if (rc != H2_PAL_OK) return rc;
  s->stats_started_ms = last;
  bool ready = false;
  for (;;) {
    if (s->config.should_exit != NULL && s->config.should_exit(s->config.user))
      return H2_PAL_OK;
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
    if (rc != H2_PAL_OK) return rc;
    const uint64_t frame_start = now;
    uint64_t dt = now > last ? now - last : 0u;
    if (dt > DT_MAX_MS) dt = DT_MAX_MS;
    last = now;

    if (s->config.reduce_motion != NULL)
      s->game.reduce_motion = s->config.reduce_motion(s->config.user);
    const bool was_paused = h2_garden_guard_paused(&s->game);
    rc = poll_touch(s);
    if (rc != H2_PAL_OK) return rc;
    if (s->config.back_pressed != NULL &&
        s->config.back_pressed(s->config.user) &&
        !h2_garden_guard_game_back(&s->game) &&
        s->config.exit_on_unhandled_back)
      return H2_PAL_OK;
    if (!was_paused) h2_garden_guard_game_step(&s->game, (uint32_t)dt);
    save_language(s);
    save_profile(s, now);
    rc = render_frame(s);
    if (rc != H2_PAL_OK) return rc;
    ++s->frames;
    ++s->frames_since_stats;
    const uint64_t finished = clock_ms(s);
    const uint64_t worked =
        finished > frame_start ? finished - frame_start : 0u;
    if (worked > s->max_frame_ms) s->max_frame_ms = (uint32_t)worked;
    if (worked > period) ++s->slow_frames;

    if (!ready) {
      ready = true;
      rc = on_first_frame(s);
      if (rc != H2_PAL_OK) return rc;
    }

    uint32_t best = 0;
    if (h2_garden_guard_game_take_best(&s->game, &best)) save_best(s, best);
    report_stats(s, now);

    const uint32_t wait_ms =
        h2_garden_guard_journal_animating(&s->game) && period > 24u ? 24u
                                                                    : period;
    rc = wait_frame(s, frame_start + wait_ms);
    if (rc != H2_PAL_OK) return rc;
  }
}

static h2_pal_result_t prepare(session_t* s) {
  h2_pal_result_t rc =
      h2_pal_display_get_info(s->runtime->display, &s->display);
  if (rc != H2_PAL_OK) return rc;
  if (s->display.native_format != H2_DISPLAY_PIXEL_RGB565 ||
      s->display.width < H2_GARDEN_GUARD_MIN_EXTENT ||
      s->display.width > H2_GARDEN_GUARD_MAX_EXTENT ||
      s->display.height < H2_GARDEN_GUARD_MIN_EXTENT ||
      s->display.height > H2_GARDEN_GUARD_MAX_EXTENT)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_touch_info_t touch = {0};
  rc = h2_pal_touch_get_info(s->runtime->touch, &touch);
  if (rc != H2_PAL_OK) return rc;
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
  s->depth = h2_pal_mem_alloc(s->allocator, bytes / sizeof(uint16_t));
  if (s->frame == NULL || s->background == NULL || s->static_layer == NULL ||
      s->depth == NULL) {
    char message[96];
    (void)snprintf(message, sizeof(message),
                   "H2_GARDEN_GUARD_ALLOC_FAIL what=frames bytes=%u",
                   (unsigned)bytes);
    log_line(s, H2_PAL_LOG_ERROR, message);
    return H2_PAL_ERR_NO_MEMORY;
  }

  uint32_t seed = s->config.seed;
  if (seed == 0u) {
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
    if (rc != H2_PAL_OK) return rc;
    seed = (uint32_t)(now ^ (now >> 32)) * 2654435761u ^ 0x9E3779B9u;
    if (seed == 0u) seed = 1u;
  }
  h2_garden_guard_game_init(&s->game, s->display.width, s->display.height, seed,
                            load_best(s));
  load_profile(s);
  load_language(s);
  notify_language(s);
  h2_garden_guard_canvas_t canvas = layer_canvas(s, s->background, false);
  h2_garden_guard_render_background(&canvas, &s->game);
  s->first_frame = true;
  return H2_PAL_OK;
}

h2_pal_result_t h2_garden_guard_run(h2_runtime_t* runtime,
                                    const h2_garden_guard_config_t* config) {
  if (runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t* allocator = runtime->mem;
  session_t* s = h2_pal_mem_alloc(allocator, sizeof(*s));
  if (s == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  s->runtime = runtime;
  s->allocator = allocator;
  if (config != NULL) s->config = *config;

  bool display_open = false, touch_open = false;
  h2_pal_result_t rc = h2_pal_display_open(runtime->display);
  if (rc == H2_PAL_OK) {
    display_open = true;
    rc = h2_pal_touch_open(runtime->touch);
  }
  if (rc == H2_PAL_OK) {
    touch_open = true;
    rc = prepare(s);
  }
  if (rc == H2_PAL_OK) rc = play(s);

  char message[64];
  (void)snprintf(message, sizeof(message),
                 "H2_GARDEN_GUARD_EXIT rc=%d frames=%u", rc,
                 (unsigned)s->frames);
  log_line(s, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR, message);

  if (touch_open) {
    const h2_pal_result_t closed = h2_pal_touch_close(runtime->touch);
    if (rc == H2_PAL_OK) rc = closed;
  }
  if (display_open) {
    const int closed = h2_pal_display_close(runtime->display);
    if (rc == H2_PAL_OK) rc = closed;
  }
  h2_pal_mem_free(allocator, s->depth);
  h2_pal_mem_free(allocator, s->static_layer);
  h2_pal_mem_free(allocator, s->background);
  h2_pal_mem_free(allocator, s->frame);
  h2_pal_mem_free(allocator, s);
  return rc;
}
