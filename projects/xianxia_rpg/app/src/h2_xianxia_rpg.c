#include "h2_xianxia_rpg.h"

#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_render.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "xianxia-rpg"
#define TOUCH_EVENTS_PER_FRAME 16u
#define TOUCH_FAILURE_LIMIT 30u
#define EVENTS_PER_DRAIN 64u
#define DT_MAX_MS 50u
#define STATS_PERIOD_MS 10000u

static const char* const k_save_keys[H2_XIANXIA_RPG_SAVE_WORDS] = {
    H2_XIANXIA_RPG_PREF_SAVE0_KEY,
    H2_XIANXIA_RPG_PREF_SAVE1_KEY,
    H2_XIANXIA_RPG_PREF_SAVE2_KEY,
    H2_XIANXIA_RPG_PREF_SAVE3_KEY,
};

/* Changed bands and, per band, the half-open column range that changed. */
typedef struct dirty {
  uint64_t bands;
  int16_t x0[H2_XIANXIA_RPG_MAX_BANDS], x1[H2_XIANXIA_RPG_MAX_BANDS];
} dirty_t;

typedef struct session {
  h2_runtime_t* runtime;
  h2_xianxia_rpg_config_t config;
  const h2_pal_mem_api_t* allocator;
  h2_display_info_t display;
  float touch_scale_x, touch_scale_y;
  uint16_t* frame;
  uint16_t* background;
  /* Background plus static panels; the scene is composed on top of it. */
  uint16_t* static_layer;
  h2_xianxia_rpg_layer_key_t background_key, static_key;
  dirty_t static_cover, previous;
  bool first_frame, rebuild_static;
  unsigned touch_failures;
  uint32_t frames, frames_since_stats;
  uint64_t stats_started_ms;
  /* Per stats window: compose and push time, and pushed pixels. */
  uint64_t compose_ms, push_ms, pixels_pushed;
  uint32_t max_frame_ms, slow_frames;
  /* Last logged state, so transitions are logged once. */
  int32_t logged[7];
  /* Fingerprint of the stored save0..save3 words, kept in each journey. */
  uint32_t chapter1_tag;
  h2_xianxia_rpg_game_t game;
  uint8_t event_payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
} session_t;

static void log_line(const session_t* s, h2_pal_log_level_t level,
                     const char* message) {
  (void)h2_pal_log_write(s->runtime->log, level, LOG_TAG, message);
}

/* Optional persistence: any failure starts without that record. The
 * chapter-1 journey uses the save0..save3 words shared with v0.1; later
 * chapters and the chapter record are single blobs, written atomically. */
static void load_records(session_t* s, h2_xianxia_rpg_records_t* out) {
  memset(out, 0, sizeof(*out));
  h2_pal_pref_namespace_t* ns = NULL;
  uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS] = {0};
  unsigned present = 0;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_XIANXIA_RPG_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  int journey_rc = rc, chapters_rc = rc;
  bool migrated_hint = false;
  if (rc == H2_PAL_OK && ns != NULL) {
    for (size_t i = 0; i < H2_XIANXIA_RPG_SAVE_WORDS && rc == H2_PAL_OK; ++i) {
      rc = ns->get_u32 != NULL ? ns->get_u32(ns, k_save_keys[i], &words[i])
                               : H2_PAL_ERR_UNSUPPORTED;
      if (rc == H2_PAL_OK) present |= 1u << i;
      /* Statistics are optional: saves made before them stay valid. */
      if (rc == H2_PAL_ERR_NOT_FOUND &&
          i >= H2_XIANXIA_RPG_SAVE_REQUIRED_WORDS) {
        words[i] = 0u;
        rc = H2_PAL_OK;
      }
    }
    /* Keys found before a missing required one still mark a record. */
    for (size_t i = 0;
         i < H2_XIANXIA_RPG_SAVE_REQUIRED_WORDS && rc != H2_PAL_OK; ++i) {
      uint32_t probe = 0;
      if (ns->get_u32 != NULL &&
          ns->get_u32(ns, k_save_keys[i], &probe) == H2_PAL_OK)
        present |= 1u << i;
    }
    void* data = NULL;
    size_t size = 0;
    journey_rc =
        ns->get_blob != NULL
            ? ns->get_blob(ns, s->allocator, H2_XIANXIA_RPG_PREF_JOURNEY_KEY,
                           &data, &size)
            : H2_PAL_ERR_UNSUPPORTED;
    uint32_t journey_tag = 0;
    if (journey_rc == H2_PAL_OK) {
      out->has_journey = h2_xianxia_rpg_journey_decode(
          data, size, &out->journey, &journey_tag);
      if (!out->has_journey) out->unreadable = true;
    }
    h2_pal_mem_free(s->allocator, data);
    data = NULL;
    size = 0;
    chapters_rc =
        ns->get_blob != NULL
            ? ns->get_blob(ns, s->allocator, H2_XIANXIA_RPG_PREF_CHAPTERS_KEY,
                           &data, &size)
            : H2_PAL_ERR_UNSUPPORTED;
    if (chapters_rc == H2_PAL_OK &&
        !h2_xianxia_rpg_progress_decode(data, size, &out->progress)) {
      memset(&out->progress, 0, sizeof(out->progress));
      out->unreadable = true;
    }
    h2_pal_mem_free(s->allocator, data);
    if (ns->close != NULL) (void)ns->close(ns);
    s->chapter1_tag = h2_xianxia_rpg_chapter1_tag(words, present);
    out->journey_latest = out->has_journey && journey_tag == s->chapter1_tag;
    migrated_hint = (out->progress.cleared & 1u) == 0u;
  }
  out->has_chapter1 =
      rc == H2_PAL_OK && h2_xianxia_rpg_save_unpack(words, &out->chapter1);
  if (present != 0u && !out->has_chapter1) out->unreadable = true;
  char message[96];
  (void)snprintf(message, sizeof(message),
                 "H2_XIANXIA_RPG_SAVE_LOAD valid=%d stage=%d rc=%d",
                 out->has_chapter1 ? 1 : 0,
                 out->has_chapter1 ? (int)out->chapter1.stage : -1, rc);
  log_line(s, H2_PAL_LOG_INFO, message);
  char records[320];
  (void)snprintf(records, sizeof(records),
                 "H2_XIANXIA_RPG_RECORDS_LOAD chapter1=%d keys=0x%x journey=%d "
                 "journey_chapter=%d journey_stage=%d latest=%d cleared=0x%x "
                 "unreadable=%d journey_rc=%d chapters_rc=%d v1_only_clear=%d",
                 out->has_chapter1 ? 1 : 0, present, out->has_journey ? 1 : 0,
                 out->has_journey ? (int)out->journey.chapter : -1,
                 out->has_journey ? (int)out->journey.stage : -1,
                 out->journey_latest ? 1 : 0, (unsigned)out->progress.cleared,
                 out->unreadable ? 1 : 0, journey_rc, chapters_rc,
                 migrated_hint && out->has_chapter1 &&
                         out->chapter1.stage == H2_XIANXIA_RPG_STAGE_CLEARED
                     ? 1
                     : 0);
  log_line(s, H2_PAL_LOG_INFO, records);
}

static void store_writes(session_t* s, const h2_xianxia_rpg_writes_t* w) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_XIANXIA_RPG_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  int chapter1_rc = rc, journey_rc = rc, progress_rc = rc;
  if (rc == H2_PAL_OK && ns != NULL) {
    if (w->chapter1) {
      uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
      h2_xianxia_rpg_save_pack(&w->chapter1_save, words);
      chapter1_rc = H2_PAL_OK;
      for (size_t i = 0;
           i < H2_XIANXIA_RPG_SAVE_WORDS && chapter1_rc == H2_PAL_OK; ++i)
        chapter1_rc = ns->set_u32 != NULL
                          ? ns->set_u32(ns, k_save_keys[i], words[i])
                          : H2_PAL_ERR_UNSUPPORTED;
      /* Later journeys compare this tag to tell which was saved last. */
      if (chapter1_rc == H2_PAL_OK)
        s->chapter1_tag = h2_xianxia_rpg_chapter1_tag(words, 0xFu);
    }
    if (w->journey) {
      uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
      h2_xianxia_rpg_journey_encode(&w->journey_save, s->chapter1_tag, bytes);
      journey_rc = ns->set_blob != NULL
                       ? ns->set_blob(ns, H2_XIANXIA_RPG_PREF_JOURNEY_KEY,
                                      bytes, sizeof(bytes))
                       : H2_PAL_ERR_UNSUPPORTED;
    }
    if (w->progress) {
      uint8_t bytes[H2_XIANXIA_RPG_PROGRESS_BYTES];
      h2_xianxia_rpg_progress_encode(&w->progress_record, bytes);
      progress_rc = ns->set_blob != NULL
                        ? ns->set_blob(ns, H2_XIANXIA_RPG_PREF_CHAPTERS_KEY,
                                       bytes, sizeof(bytes))
                        : H2_PAL_ERR_UNSUPPORTED;
    }
    rc = ns->commit != NULL ? ns->commit(ns) : H2_PAL_OK;
    if (ns->close != NULL) {
      const int closed = ns->close(ns);
      if (rc == H2_PAL_OK) rc = closed;
    }
  }
  char message[160];
  if (w->chapter1) {
    (void)snprintf(message, sizeof(message),
                   "H2_XIANXIA_RPG_SAVE_STORE stage=%u level=%u map=%u rc=%d",
                   (unsigned)w->chapter1_save.stage,
                   (unsigned)w->chapter1_save.level,
                   (unsigned)w->chapter1_save.map,
                   chapter1_rc != H2_PAL_OK ? chapter1_rc : rc);
    log_line(s,
             chapter1_rc == H2_PAL_OK && rc == H2_PAL_OK ? H2_PAL_LOG_INFO
                                                         : H2_PAL_LOG_WARN,
             message);
  }
  if (w->journey) {
    (void)snprintf(
        message, sizeof(message),
        "H2_XIANXIA_RPG_JOURNEY_STORE chapter=%u stage=%u level=%u "
        "map=%u x=%u y=%u rc=%d",
        (unsigned)w->journey_save.chapter, (unsigned)w->journey_save.stage,
        (unsigned)w->journey_save.level, (unsigned)w->journey_save.map,
        (unsigned)w->journey_save.x, (unsigned)w->journey_save.y,
        journey_rc != H2_PAL_OK ? journey_rc : rc);
    log_line(s,
             journey_rc == H2_PAL_OK && rc == H2_PAL_OK ? H2_PAL_LOG_INFO
                                                        : H2_PAL_LOG_WARN,
             message);
  }
  if (w->progress) {
    (void)snprintf(message, sizeof(message),
                   "H2_XIANXIA_RPG_PROGRESS_STORE cleared=0x%x rc=%d",
                   (unsigned)w->progress_record.cleared,
                   progress_rc != H2_PAL_OK ? progress_rc : rc);
    log_line(s,
             progress_rc == H2_PAL_OK && rc == H2_PAL_OK ? H2_PAL_LOG_INFO
                                                         : H2_PAL_LOG_WARN,
             message);
  }
}

/* One line per phase, map, stage, battle-state or dialogue-line change. */
static void log_transitions(session_t* s) {
  const h2_xianxia_rpg_game_t* g = &s->game;
  /* Walking starts and stops are logged too, with the cell reached. */
  const bool walking = g->moving || g->path_pos < g->path_len;
  const int32_t now[7] = {
      (int32_t)g->phase,
      (int32_t)g->map,
      (int32_t)g->stage,
      g->phase == H2_XIANXIA_RPG_PHASE_BATTLE ? (int32_t)g->battle.state : -1,
      g->dialogue.active ? (int32_t)g->dialogue.index : -1,
      (int32_t)g->flags,
      walking ? 1 : 0};
  if (memcmp(now, s->logged, sizeof(now)) == 0) return;
  memcpy(s->logged, now, sizeof(now));
  const int32_t enemy = now[3] >= 0 ? (int32_t)g->battle.enemy : -1;
  const h2_xianxia_rpg_battle_t* b = &g->battle;
  char message[512];
  /* int32_t is long on some embedded toolchains, so use its own format. */
  (void)snprintf(
      message, sizeof(message),
      "H2_XIANXIA_RPG_STATE phase=%" PRId32 " map=%" PRId32 " stage=%" PRId32
      " battle=%" PRId32 " dialogue=%" PRId32
      " level=%u hp=%u/%u mp=%u pills=%u enemy=%" PRId32
      " enemy_hp=%u charging=%d won=%u turns=%u chapter=%u"
      " x=%" PRId32 " y=%" PRId32 " rest=%u,%" PRId32 ",%" PRId32
      " flags=0x%x trust=%u ally=%d rapport=%u veiled=%d"
      " sealed=%d split=%d real=%u joint=%d walking=%d",
      now[0], now[1], now[2], now[3], now[4], (unsigned)g->hero.level,
      (unsigned)g->hero.hp, (unsigned)g->hero.hp_max, (unsigned)g->hero.mp,
      (unsigned)g->hero.pills, enemy, (unsigned)b->enemy_hp,
      b->charging ? 1 : 0, (unsigned)g->battles_won, (unsigned)g->turns_taken,
      (unsigned)g->chapter, g->tx, g->ty, (unsigned)g->rest_map, g->rest_x,
      g->rest_y, (unsigned)g->flags, (unsigned)g->trust, b->ally ? 1 : 0,
      (unsigned)b->rapport, b->veiled ? 1 : 0, b->mp_sealed ? 1 : 0,
      b->split ? 1 : 0, (unsigned)b->real_shadow,
      h2_xianxia_rpg_joint_ready(g) ? 1 : 0, walking ? 1 : 0);
  log_line(s, H2_PAL_LOG_INFO, message);
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
                       "H2_XIANXIA_RPG_TOUCH_RETRY rc=%d", rc);
        log_line(s, H2_PAL_LOG_WARN, message);
      }
      h2_xianxia_rpg_game_pointer(&s->game, H2_XIANXIA_RPG_POINTER_CANCEL, 0.0f,
                                  0.0f);
      return s->touch_failures >= TOUCH_FAILURE_LIMIT ? rc : H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) return rc;
    if (s->touch_failures != 0u) {
      s->touch_failures = 0u;
      log_line(s, H2_PAL_LOG_INFO, "H2_XIANXIA_RPG_TOUCH_RECOVERED");
    }
    h2_xianxia_rpg_pointer_t kind;
    switch (event.kind) {
      case H2_PAL_TOUCH_EVENT_DOWN:
        kind = H2_XIANXIA_RPG_POINTER_DOWN;
        break;
      case H2_PAL_TOUCH_EVENT_MOVE:
        kind = H2_XIANXIA_RPG_POINTER_MOVE;
        break;
      case H2_PAL_TOUCH_EVENT_UP:
        kind = H2_XIANXIA_RPG_POINTER_UP;
        break;
      default:
        continue;
    }
    h2_xianxia_rpg_game_pointer(&s->game, kind,
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

static h2_xianxia_rpg_canvas_t canvas_for(const session_t* s, uint16_t* pixels,
                                          bool measure) {
  h2_xianxia_rpg_canvas_t canvas;
  memset(&canvas, 0, sizeof(canvas));
  canvas.pixels = pixels;
  canvas.width = s->display.width;
  canvas.height = s->display.height;
  canvas.stride_pixels = (size_t)s->display.width;
  canvas.measure = measure;
  return canvas;
}

static void dirty_from_canvas(dirty_t* out,
                              const h2_xianxia_rpg_canvas_t* canvas) {
  out->bands = canvas->bands;
  memcpy(out->x0, canvas->band_x0, sizeof(out->x0));
  memcpy(out->x1, canvas->band_x1, sizeof(out->x1));
}

static void dirty_merge(dirty_t* into, const dirty_t* other) {
  for (int32_t band = 0; band < H2_XIANXIA_RPG_MAX_BANDS; ++band) {
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
  out->bands = h2_xianxia_rpg_all_bands(s->display.height);
  for (int32_t band = 0; band < H2_XIANXIA_RPG_MAX_BANDS; ++band) {
    out->x0[band] = 0;
    out->x1[band] = (int16_t)s->display.width;
  }
}

/* Copy every dirty column range from source to destination. */
static void copy_dirty(const session_t* s, uint16_t* destination,
                       const uint16_t* source, const dirty_t* dirty) {
  const int32_t width = s->display.width, height = s->display.height;
  for (int32_t band = 0; band < H2_XIANXIA_RPG_MAX_BANDS; ++band) {
    if (!(dirty->bands & ((uint64_t)1 << band))) continue;
    const int32_t y0 = band * H2_XIANXIA_RPG_BAND_ROWS;
    if (y0 >= height) break;
    int32_t y1 = y0 + H2_XIANXIA_RPG_BAND_ROWS;
    if (y1 > height) y1 = height;
    const int32_t x0 = dirty->x0[band], x1 = dirty->x1[band];
    for (int32_t y = y0; y < y1; ++y) {
      const size_t offset = (size_t)y * (size_t)width + (size_t)x0;
      memcpy(destination + offset, source + offset,
             (size_t)(x1 - x0) * sizeof(uint16_t));
    }
  }
}

/* Repaint the background when its key changed; everything is then dirty. */
static bool update_background(session_t* s) {
  h2_xianxia_rpg_layer_key_t key;
  h2_xianxia_rpg_background_key(&s->game, &key);
  if (!s->first_frame && memcmp(&key, &s->background_key, sizeof(key)) == 0)
    return false;
  h2_xianxia_rpg_canvas_t canvas = canvas_for(s, s->background, false);
  h2_xianxia_rpg_render_background(&canvas, &s->game);
  s->background_key = key;
  return true;
}

/* Rebuild the static layer when its key changed, adding what changed. */
static void update_static_layer(session_t* s, bool everything,
                                dirty_t* changed) {
  h2_xianxia_rpg_layer_key_t key;
  h2_xianxia_rpg_static_key(&s->game, &key);
  if (!everything && memcmp(&key, &s->static_key, sizeof(key)) == 0) return;
  h2_xianxia_rpg_canvas_t canvas = canvas_for(s, s->static_layer, true);
  h2_xianxia_rpg_render_static(&canvas, &s->game);
  dirty_t current;
  dirty_from_canvas(&current, &canvas);
  if (everything) {
    dirty_all(changed, s);
  } else {
    dirty_merge(changed, &current);
    dirty_merge(changed, &s->static_cover);
  }
  copy_dirty(s, s->static_layer, s->background, changed);
  canvas.measure = false;
  canvas.bands = 0;
  h2_xianxia_rpg_render_static(&canvas, &s->game);
  s->static_key = key;
  s->static_cover = current;
}

/* Merge only equal column spans, widened to even columns for panels that
 * need them. */
static h2_pal_result_t push_dirty(session_t* s, const dirty_t* dirty) {
  const int32_t width = s->display.width, height = s->display.height;
  const int32_t band_count =
      (height + H2_XIANXIA_RPG_BAND_ROWS - 1) / H2_XIANXIA_RPG_BAND_ROWS;
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
    const int32_t y0 = start * H2_XIANXIA_RPG_BAND_ROWS;
    int32_t y1 = band * H2_XIANXIA_RPG_BAND_ROWS;
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

static h2_pal_result_t render_frame(session_t* s) {
  const uint64_t started = clock_ms(s);
  dirty_t dirty = {0};
  const bool everything = update_background(s) || s->rebuild_static;
  update_static_layer(s, everything, &dirty);
  h2_xianxia_rpg_canvas_t canvas = canvas_for(s, s->frame, true);
  h2_xianxia_rpg_render_scene(&canvas, &s->game);
  dirty_t current;
  dirty_from_canvas(&current, &canvas);
  dirty_merge(&dirty, &current);
  dirty_merge(&dirty, &s->previous);
  copy_dirty(s, s->frame, s->static_layer, &dirty);
  canvas.measure = false;
  canvas.bands = 0;
  h2_xianxia_rpg_render_scene(&canvas, &s->game);
  const uint64_t composed = clock_ms(s);
  const h2_pal_result_t rc = push_dirty(s, &dirty);
  const uint64_t pushed = clock_ms(s);
  s->compose_ms += composed - started;
  s->push_ms += pushed - composed;
  /* A failed push keeps the full rebuild pending for the next frame. */
  s->rebuild_static = rc != H2_PAL_OK;
  if (rc == H2_PAL_OK) {
    s->previous = current;
    s->first_frame = false;
  }
  return rc;
}

/* Wait at least once, until the frame deadline, draining Runtime events. */
static h2_pal_result_t wait_frame(session_t* s, uint64_t deadline_ms) {
  for (;;) {
    uint64_t now = 0;
    h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
    if (rc != H2_PAL_OK) return rc;
    const uint64_t remaining = now < deadline_ms ? deadline_ms - now : 0u;
    rc = h2_runtime_wait_notify(s->runtime,
                                remaining == 0u ? 1u : (uint32_t)remaining);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT) return rc;
    const h2_pal_result_t drained = drain_events(s);
    if (drained != H2_PAL_OK) return drained;
    if (rc == H2_PAL_ERR_TIMEOUT || remaining == 0u) return H2_PAL_OK;
  }
}

static void report_stats(session_t* s, uint64_t now) {
  const uint64_t elapsed = now - s->stats_started_ms;
  if (elapsed < STATS_PERIOD_MS) return;
  const uint64_t tenths = (uint64_t)s->frames_since_stats * 10000u / elapsed;
  const uint64_t frames = s->frames_since_stats ? s->frames_since_stats : 1u;
  const uint64_t compose = s->compose_ms * 10u / frames;
  const uint64_t push = s->push_ms * 10u / frames;
  char message[320];
  (void)snprintf(
      message, sizeof(message),
      "H2_XIANXIA_RPG_STATS fps=%u.%u frames=%u phase=%u map=%u stage=%u "
      "level=%u compose_ms=%u.%u push_ms=%u.%u kpx=%u max_ms=%u slow=%u",
      (unsigned)(tenths / 10u), (unsigned)(tenths % 10u), (unsigned)s->frames,
      (unsigned)s->game.phase, (unsigned)s->game.map, (unsigned)s->game.stage,
      (unsigned)s->game.hero.level, (unsigned)(compose / 10u),
      (unsigned)(compose % 10u), (unsigned)(push / 10u), (unsigned)(push % 10u),
      (unsigned)(s->pixels_pushed / frames / 1000u), (unsigned)s->max_frame_ms,
      (unsigned)s->slow_frames);
  log_line(s, H2_PAL_LOG_INFO, message);
  s->stats_started_ms = now;
  s->frames_since_stats = 0;
  s->compose_ms = s->push_ms = s->pixels_pushed = 0;
  s->max_frame_ms = s->slow_frames = 0;
}

static h2_pal_result_t first_frame_ready(session_t* s) {
  if (s->config.brightness_percent != 0u) {
    const uint32_t percent = s->config.brightness_percent > 100u
                                 ? 100u
                                 : s->config.brightness_percent;
    const int brightness =
        h2_pal_display_set_brightness_percent(s->runtime->display, percent);
    if (brightness != H2_PAL_OK) {
      char message[64];
      (void)snprintf(message, sizeof(message),
                     "H2_XIANXIA_RPG_BRIGHTNESS rc=%d", brightness);
      log_line(s, H2_PAL_LOG_WARN, message);
    }
  }
  char message[96];
  (void)snprintf(message, sizeof(message),
                 "H2_XIANXIA_RPG_READY width=%d height=%d", s->display.width,
                 s->display.height);
  log_line(s, H2_PAL_LOG_INFO, message);
  return s->config.ready != NULL ? s->config.ready(s->config.user) : H2_PAL_OK;
}

static h2_pal_result_t play(session_t* s) {
  const uint32_t period = s->config.frame_period_ms != 0u
                              ? s->config.frame_period_ms
                              : H2_XIANXIA_RPG_DEFAULT_FRAME_PERIOD_MS;
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

    rc = poll_touch(s);
    if (rc != H2_PAL_OK) return rc;
    h2_xianxia_rpg_game_step(&s->game, (uint32_t)dt);
    log_transitions(s);
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
      rc = first_frame_ready(s);
      if (rc != H2_PAL_OK) return rc;
    }

    h2_xianxia_rpg_writes_t writes;
    if (h2_xianxia_rpg_game_take_writes(&s->game, &writes))
      store_writes(s, &writes);
    report_stats(s, now);

    rc = wait_frame(s, frame_start + period);
    if (rc != H2_PAL_OK) return rc;
  }
}

static h2_pal_result_t prepare(session_t* s) {
  h2_pal_result_t rc =
      h2_pal_display_get_info(s->runtime->display, &s->display);
  if (rc != H2_PAL_OK) return rc;
  if (s->display.native_format != H2_DISPLAY_PIXEL_RGB565 ||
      s->display.width < H2_XIANXIA_RPG_MIN_WIDTH ||
      s->display.width > H2_XIANXIA_RPG_MAX_EXTENT ||
      s->display.height < H2_XIANXIA_RPG_MIN_HEIGHT ||
      s->display.height > H2_XIANXIA_RPG_MAX_EXTENT)
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
  if (s->frame == NULL || s->background == NULL || s->static_layer == NULL) {
    char message[96];
    (void)snprintf(message, sizeof(message),
                   "H2_XIANXIA_RPG_ALLOC_FAIL what=frames bytes=%u",
                   (unsigned)bytes);
    log_line(s, H2_PAL_LOG_ERROR, message);
    return H2_PAL_ERR_NO_MEMORY;
  }

  uint32_t seed = s->config.seed;
  if (seed == 0u) {
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
    if (rc != H2_PAL_OK) return rc;
    seed = (uint32_t)(now ^ (now >> 32)) * 2654435761u ^ 0x51DE5EEDu;
    if (seed == 0u) seed = 1u;
  }
  h2_xianxia_rpg_records_t records;
  load_records(s, &records);
  h2_xianxia_rpg_game_init(&s->game, s->display.width, s->display.height, seed,
                           &records);
  s->first_frame = true;
  for (size_t i = 0; i < sizeof(s->logged) / sizeof(s->logged[0]); ++i)
    s->logged[i] = -2;
  return H2_PAL_OK;
}

h2_pal_result_t h2_xianxia_rpg_run(h2_runtime_t* runtime,
                                   const h2_xianxia_rpg_config_t* config) {
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
                 "H2_XIANXIA_RPG_EXIT rc=%d frames=%u", rc,
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
  h2_pal_mem_free(allocator, s->static_layer);
  h2_pal_mem_free(allocator, s->background);
  h2_pal_mem_free(allocator, s->frame);
  h2_pal_mem_free(allocator, s);
  return rc;
}
