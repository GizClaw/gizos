#ifndef H2_GARDEN_GUARD_H
#define H2_GARDEN_GUARD_H

#include "h2_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default frame period: a 25 fps cap, ample for walking pests. */
#define H2_GARDEN_GUARD_DEFAULT_FRAME_PERIOD_MS 40u

/** Supported Display/Touch viewport bounds, inclusive. */
#define H2_GARDEN_GUARD_MIN_EXTENT 160
#define H2_GARDEN_GUARD_MAX_EXTENT 1024

/** Pref namespace and key holding the persisted best score (u32). */
#define H2_GARDEN_GUARD_PREF_NAMESPACE "garden_guard"
#define H2_GARDEN_GUARD_PREF_BEST_KEY "best"

typedef enum h2_garden_guard_cover_action {
  H2_GARDEN_GUARD_COVER_START = 1,
  H2_GARDEN_GUARD_COVER_JOURNAL,
  H2_GARDEN_GUARD_COVER_SHOP,
  H2_GARDEN_GUARD_COVER_LANGUAGE,
  H2_GARDEN_GUARD_COVER_ACTION_COUNT,
} h2_garden_guard_cover_action_t;

typedef struct h2_garden_guard_cover_rect {
  float x, y, width, height; /**< Fractions of the display rectangle. */
} h2_garden_guard_cover_rect_t;

/* Geometry and exact pose for an optional baked plant cel. The provider can
 * return NULL to let the game draw the original vector art. */
typedef struct h2_garden_guard_plant_span_key {
  uint32_t lane, column;
  uint8_t kind;
  bool blink;
  uint64_t idle_tick;
  float x, y, scale, sway;
} h2_garden_guard_plant_span_key_t;

typedef struct h2_garden_guard_config {
  /** Nonzero fixes the random seed; zero derives one from the clock. */
  uint32_t seed;
  /** Backlight percent applied after the first presented frame, 1..100.
   * Zero leaves the provider setting. Brightness is optional: a failure is
   * logged and the game continues. */
  uint32_t brightness_percent;
  /** Frame period in milliseconds; zero selects the default. */
  uint32_t frame_period_ms;
  /** Optional, called once on the run thread after the first frame was
   * presented with Touch open. A nonzero result ends the run with it. */
  h2_pal_result_t (*ready)(void* user);
  /** Optional, polled on the run thread before every frame. Returning true
   * ends the run with H2_PAL_OK after cleanup. */
  bool (*should_exit)(void* user);
  /** Optional notification on the run thread after loading or switching
   * language. language_tag is a borrowed static BCP 47 tag. */
  void (*language_changed)(void* user, const char* language_tag);
  /** Optional input adapter. Return true once for a requested Back action. */
  bool (*back_pressed)(void* user);
  /** When true, a Back action outside a menu exits the app. */
  bool exit_on_unhandled_back;
  /** Optional platform preference. If true, show the journal detail
   * immediately. */
  bool (*reduce_motion)(void* user);
  /** Optional platform cover label layer. When present the App paints art
   * only on the cover. Controls are borrowed for this callback on the run
   * thread. A NULL controls pointer hides the layer. Entries begin at 1. */
  void (*cover_changed)(void* user,
                        const h2_garden_guard_cover_rect_t* controls,
                        bool session_language);
  /** Optional one-shot input from the cover layer, zero when idle. */
  h2_garden_guard_cover_action_t (*cover_action)(void* user);
  /** Use up to 560 KiB of optional scratch space to reuse exact plant pixels.
   * Allocation failure leaves the normal renderer active. */
  bool cache_plants;
  /** Optional trusted, immutable compact plant spans for the native 480x320
   * lawn. See the project README for the byte format. Return NULL for any
   * unavailable key. The data must stay live until h2_garden_guard_run ends. */
  const uint8_t* (*plant_spans_find)(
      void* user, const h2_garden_guard_plant_span_key_t* key);
  /** Optional bounded asset preparation on the run thread after present. The
   * game calls it in idle time on the title or through wave two. One call
   * must process at most one small block and perform no external I/O. */
  void (*plant_spans_prepare_step)(void* user);
  /** Borrowed user value passed to the configured callbacks. */
  void* user;
} h2_garden_guard_config_t;

/**
 * Run the lane-defense garden game on the calling thread until should_exit()
 * returns true or a required capability fails.
 *
 * Requires an initialized Runtime whose Display reports RGB565 within the
 * supported extent and whose Touch provider is available; Touch coordinates
 * are scaled to the Display extent. The lawn is drawn in perspective from
 * its front edge with lanes running right to left on every display shape;
 * taller displays show more courtyard behind it. The run opens and closes
 * Display and Touch, drains Runtime events, and paces frames with
 * h2_runtime_wait_notify(). Pref is optional: when unavailable the best score
 * is kept in memory only. Game state, three RGB565 frame-sized buffers and a
 * one-byte-per-pixel occlusion map come from Runtime mem before the first
 * frame; frames never allocate. Diagnostics
 * use Runtime log. Every allocation and provider session ends before return.
 * The config is borrowed for the call; NULL selects defaults.
 *
 * @return H2_PAL_OK after a requested exit; INVALID_ARG for a NULL Runtime;
 *         UNSUPPORTED for an unusable Display format or extent; NO_MEMORY;
 *         or the first Display, Touch, clock, Runtime wait or ready error.
 */
h2_pal_result_t h2_garden_guard_run(h2_runtime_t* runtime,
                                    const h2_garden_guard_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
