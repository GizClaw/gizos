#ifndef H2_FRUIT_SLICE_H
#define H2_FRUIT_SLICE_H

#include "h2_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default frame period: a 40 fps cap. */
#define H2_FRUIT_SLICE_DEFAULT_FRAME_PERIOD_MS 25u

/** Supported Display/Touch viewport bounds, inclusive. */
#define H2_FRUIT_SLICE_MIN_EXTENT 160
#define H2_FRUIT_SLICE_MAX_EXTENT 1024

/** Pref namespace and key holding the persisted best score (u32). */
#define H2_FRUIT_SLICE_PREF_NAMESPACE "fruit_slice"
#define H2_FRUIT_SLICE_PREF_BEST_KEY "best"

typedef struct h2_fruit_slice_config {
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
  /** Borrowed user value passed to ready and should_exit. */
  void* user;
} h2_fruit_slice_config_t;

/**
 * Run the fruit-slicing game on the calling thread until should_exit() returns
 * true or a required capability fails.
 *
 * Requires an initialized Runtime whose Display reports RGB565 within the
 * supported extent and whose Touch provider is available; Touch coordinates
 * are scaled to the Display extent. The run opens and closes Display and
 * Touch, drains Runtime events, and paces frames with h2_runtime_wait_notify().
 * Pref is optional: when unavailable the best score is kept in memory only.
 * Game state, three RGB565 frame-sized buffers and the fruit texture cache
 * come from Runtime mem before the first frame; frames never allocate.
 * Diagnostics use Runtime log. Every allocation, provider session and
 * callback ends before return. The config is borrowed for the call; NULL
 * selects defaults.
 *
 * @return H2_PAL_OK after a requested exit; INVALID_ARG for a NULL Runtime;
 *         UNSUPPORTED for an unusable Display format or extent; NO_MEMORY;
 *         or the first Display, Touch, clock, Runtime wait or ready error.
 */
h2_pal_result_t h2_fruit_slice_run(h2_runtime_t* runtime,
                                   const h2_fruit_slice_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
