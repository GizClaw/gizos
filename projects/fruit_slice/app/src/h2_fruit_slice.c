#include "h2_fruit_slice.h"

#include "h2_fruit_slice_game.h"
#include "h2_fruit_slice_render.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "fruit-slice"
#define TOUCH_EVENTS_PER_FRAME 16u
#define TOUCH_FAILURE_LIMIT 30u
#define EVENTS_PER_DRAIN 64u
#define DT_MAX_MS 50u
#define STATS_PERIOD_MS 10000u

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
  bool first_frame;
  unsigned touch_failures;
  uint32_t frames, frames_since_stats;
  uint64_t stats_started_ms;
  /* Per stats window: compose and push time, and pushed pixels. */
  uint64_t compose_ms, push_ms, pixels_pushed;
  /* Per stats window: longest frame of work, and frames over the period. */
  uint32_t max_frame_ms, slow_frames;
  uint32_t blast_frames, blast_max_ms;
  h2_fruit_slice_game_t game;
  uint8_t event_payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
} session_t;

static void log_line(const session_t* s, h2_pal_log_level_t level,
                     const char* message) {
  (void)h2_pal_log_write(s->runtime->log, level, LOG_TAG, message);
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

/* Optional persistence: a failure keeps the in-memory best only. */
static void save_best(session_t* s, uint32_t best) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(s->runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns != NULL) {
    rc = ns->set_u32 != NULL
             ? ns->set_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, best)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK && ns->commit != NULL) rc = ns->commit(ns);
    if (ns->close != NULL) {
      const int closed = ns->close(ns);
      if (rc == H2_PAL_OK) rc = closed;
    }
  }
  char message[80];
  (void)snprintf(message, sizeof(message),
                 "H2_FRUIT_SLICE_BEST_SAVE best=%u rc=%d", (unsigned)best, rc);
  log_line(s, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_WARN, message);
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
                       "H2_FRUIT_SLICE_TOUCH_RETRY rc=%d", rc);
        log_line(s, H2_PAL_LOG_WARN, message);
      }
      /* Abandon the stroke rather than slicing along a stale segment. */
      h2_fruit_slice_game_pointer(&s->game, H2_FRUIT_SLICE_POINTER_CANCEL, 0.0f,
                                  0.0f);
      return s->touch_failures >= TOUCH_FAILURE_LIMIT ? rc : H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) return rc;
    if (s->touch_failures != 0u) {
      s->touch_failures = 0u;
      log_line(s, H2_PAL_LOG_INFO, "H2_FRUIT_SLICE_TOUCH_RECOVERED");
    }
    h2_fruit_slice_pointer_t kind;
    switch (event.kind) {
      case H2_PAL_TOUCH_EVENT_DOWN:
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
    s->pixels_pushed += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
  }
  if (rc == H2_PAL_OK && dirty->bands != 0u)
    rc = h2_pal_display_present(s->runtime->display);
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
  const h2_pal_result_t rc = push_dirty(s, &dirty);
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
    h2_fruit_slice_game_step(&s->game, (uint32_t)dt);
    rc = render_frame(s);
    if (rc != H2_PAL_OK) return rc;
    ++s->frames;
    ++s->frames_since_stats;
    const uint64_t finished = clock_ms(s);
    const uint64_t worked =
        finished > frame_start ? finished - frame_start : 0u;
    if (worked > s->max_frame_ms) s->max_frame_ms = (uint32_t)worked;
    if (worked > period) ++s->slow_frames;
    if (s->game.flash_ms != 0u) {
      ++s->blast_frames;
      if (worked > s->blast_max_ms) s->blast_max_ms = (uint32_t)worked;
    }

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
        if (rc != H2_PAL_OK) return rc;
      }
    }

    uint32_t best = 0;
    if (h2_fruit_slice_game_take_best(&s->game, &best)) save_best(s, best);
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
      s->display.width < H2_FRUIT_SLICE_MIN_EXTENT ||
      s->display.width > H2_FRUIT_SLICE_MAX_EXTENT ||
      s->display.height < H2_FRUIT_SLICE_MIN_EXTENT ||
      s->display.height > H2_FRUIT_SLICE_MAX_EXTENT)
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
    if (rc != H2_PAL_OK) return rc;
    seed = (uint32_t)(now ^ (now >> 32)) * 2654435761u ^ 0xA511E9B3u;
    if (seed == 0u) seed = 1u;
  }
  h2_fruit_slice_game_init(&s->game, s->display.width, s->display.height, seed,
                           load_best(s));
  /* Build every fixed-size texture now so frames never allocate. */
  if (!h2_fruit_slice_sprites_prepare(s->sprites, &s->game)) {
    log_line(s, H2_PAL_LOG_ERROR, "H2_FRUIT_SLICE_ALLOC_FAIL what=textures");
    return H2_PAL_ERR_NO_MEMORY;
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
                 "H2_FRUIT_SLICE_EXIT rc=%d frames=%u", rc,
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
  h2_fruit_slice_sprites_destroy(s->sprites);
  h2_pal_mem_free(allocator, s->static_layer);
  h2_pal_mem_free(allocator, s->background);
  h2_pal_mem_free(allocator, s->frame);
  h2_pal_mem_free(allocator, s);
  return rc;
}
