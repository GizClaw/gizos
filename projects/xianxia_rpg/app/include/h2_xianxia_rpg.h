#ifndef H2_XIANXIA_RPG_H
#define H2_XIANXIA_RPG_H

#include "h2_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default frame period: a 30 fps cap. */
#define H2_XIANXIA_RPG_DEFAULT_FRAME_PERIOD_MS 33u

/** Supported Display/Touch viewport bounds, inclusive. */
#define H2_XIANXIA_RPG_MIN_WIDTH 320
#define H2_XIANXIA_RPG_MIN_HEIGHT 240
#define H2_XIANXIA_RPG_MAX_EXTENT 1024

/** Pref namespace and keys holding the persisted journey (three u32). */
#define H2_XIANXIA_RPG_PREF_NAMESPACE "xianxia_rpg"
#define H2_XIANXIA_RPG_PREF_SAVE0_KEY "save0"
#define H2_XIANXIA_RPG_PREF_SAVE1_KEY "save1"
#define H2_XIANXIA_RPG_PREF_SAVE2_KEY "save2"
/** Ending statistics (battles won, turns); absent in older saves, which
 * then load with zero statistics. */
#define H2_XIANXIA_RPG_PREF_SAVE3_KEY "save3"

typedef struct h2_xianxia_rpg_config {
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
} h2_xianxia_rpg_config_t;

/**
 * Run the xianxia role-playing game on the calling thread until
 * should_exit() returns true or a required capability fails.
 *
 * Requires an initialized Runtime whose Display reports RGB565 within the
 * supported extent and whose Touch provider is available; Touch coordinates
 * are scaled to the Display extent. Every action is a tap: tap ground to
 * walk, characters to talk, and battle commands to fight. The run opens and
 * closes Display and Touch, drains Runtime events, and paces frames with
 * h2_runtime_wait_notify(). Pref is optional: when unavailable the journey is
 * kept in memory only. Game state and three RGB565 frame-sized buffers come
 * from Runtime mem before the first frame; frames never allocate.
 * Diagnostics use Runtime log. Every allocation, provider session and
 * callback ends before return. The config is borrowed for the call; NULL
 * selects defaults.
 *
 * @return H2_PAL_OK after a requested exit; INVALID_ARG for a NULL Runtime;
 *         UNSUPPORTED for an unusable Display format or extent; NO_MEMORY;
 *         or the first Display, Touch, clock, Runtime wait or ready error.
 */
h2_pal_result_t h2_xianxia_rpg_run(h2_runtime_t* runtime,
                                   const h2_xianxia_rpg_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
