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

typedef enum h2_fruit_slice_language {
  H2_FRUIT_SLICE_LANGUAGE_ZH_CN = 0, /**< Default, including zeroed configs. */
  H2_FRUIT_SLICE_LANGUAGE_EN = 1,
} h2_fruit_slice_language_t;

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
  /** Borrowed user value passed to all callbacks. */
  void* user;
  /** Optional nonblocking lifecycle query, called on the run thread. True
   * closes the audio track/speaker and pauses simulation until false. */
  bool (*is_paused)(void* user);
  /** Initial UI language; zero/unknown selects Simplified Chinese. The title
   * control switches it for this session without changing stored progress. */
  h2_fruit_slice_language_t language;
  /** Optional low-rate diagnostic hook on the run thread after each stats
   * window (10 seconds). Must be bounded/nonblocking; borrows user. */
  void (*report_diagnostics)(void* user);
  /** Native hosts may open the silent audio path during loading. No PCM is
   * submitted before the first user gesture; fully muted audio stays closed.
   * Default false keeps browser/mobile gesture-based hardware startup. */
  bool prewarm_audio;
  /** Opt into an independent PCM producer using Runtime task/queue APIs.
   * The worker owns audio state; pause/exit join it before teardown. Mute
   * controls queue without waiting for hardware or persistence. A single
   * short-lived Pref task saves coalesced settings without starving PCM.
   * Hosts declare fruit-slice/audio and fruit-slice/audio-pref task policies.
   * If optional producer startup fails, playback stays disabled and settings
   * use the cooperative persistence path, including bounded retries.
   * Default false. */
  bool audio_worker;
} h2_fruit_slice_config_t;

/**
 * Run the fruit-slicing game on the calling thread until should_exit() returns
 * true or a required capability fails.
 *
 * Requires an initialized Runtime whose Display reports RGB565 within the
 * supported extent and whose Touch provider is available; Touch coordinates
 * are scaled to the Display extent. The run opens and closes Display and
 * Touch, drains Runtime events, and paces frames with h2_runtime_wait_notify().
 * Pref is optional: when unavailable the best score, coins, task progress,
 * owned/equipped weapons and audio settings stay in memory for this call.
 * Successful claims, purchases and equipment changes request one versioned
 * profile save; failed saves are retried and pending progress is flushed
 * before cleanup. Audio is optional; an unsupported/failed provider is logged
 * and play continues. Title controls toggle SFX/BGM independently; audio starts
 * on a user gesture. is_paused must be nonblocking so audio can stop before
 * the host waits. Game state, three RGB565 frame-sized buffers and the fruit
 * texture cache come from Runtime mem before the first frame; frames never
 * allocate.
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
