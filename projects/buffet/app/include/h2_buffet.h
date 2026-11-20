#ifndef H2_BUFFET_H
#define H2_BUFFET_H

#include "h2_runtime.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BUFFET_PREF_NAMESPACE "buffet_cards_v1"
enum { H2_BUFFET_WIDTH = 800, H2_BUFFET_HEIGHT = 480, H2_BUFFET_DENSITY = 2 };

typedef enum h2_buffet_action {
  H2_BUFFET_CONFIRM = 1,
  H2_BUFFET_PREVIOUS = 2,
  H2_BUFFET_NEXT = 4,
  H2_BUFFET_GOAL = 8,
  H2_BUFFET_LANGUAGE = 16,
  H2_BUFFET_ALBUM = 32,
  H2_BUFFET_SOUND = 64,
  H2_BUFFET_MENU = 128,
} h2_buffet_action_t;

/** Optional host controls. Axes are held directions in -1..1. Actions are
 * rising edges only. An inactive host freezes the meal and clears movement. */
typedef struct h2_buffet_input {
  int dx, dy;
  unsigned actions;
  bool inactive, close;
} h2_buffet_input_t;

typedef h2_pal_result_t (*h2_buffet_card_work_fn)(void* context);

/** Entered stages of the single game task, in diagnostic wire order. */
typedef enum h2_buffet_stage {
  H2_BUFFET_STAGE_FRAME = 1,
  H2_BUFFET_STAGE_DINING_FRAME,
  H2_BUFFET_STAGE_STEP,
  H2_BUFFET_STAGE_TOUCH,
  H2_BUFFET_STAGE_PREF,
  H2_BUFFET_STAGE_AUDIO,
  H2_BUFFET_STAGE_TRACK_OPEN,
  H2_BUFFET_STAGE_MIX,
  H2_BUFFET_STAGE_TRACK_WRITE,
  H2_BUFFET_STAGE_TRACK_STOP,
  H2_BUFFET_STAGE_GROUND,
  H2_BUFFET_STAGE_COPY,
  H2_BUFFET_STAGE_SCENE,
  H2_BUFFET_STAGE_COMPARE,
  H2_BUFFET_STAGE_BITMAP,
  H2_BUFFET_STAGE_PRESENT,
  H2_BUFFET_STAGE_REPORT,
  H2_BUFFET_STAGE_WAIT,
  H2_BUFFET_STAGE_LATE,
  H2_BUFFET_STAGE_REST,
  H2_BUFFET_STAGE_REST_DONE,
  H2_BUFFET_STAGE_CLEANUP,
} h2_buffet_stage_t;

typedef struct h2_buffet_config {
  void* user;
  void (*poll)(void* user, h2_buffet_input_t* input);
  bool keyboard;
  bool reduced_motion;
  /** Called once after the first successful display presentation and touch
   * open. */
  h2_pal_result_t (*ready)(void* user);
  /** Optional host serialization for a shared Pref save. Invoke work once
   * while holding the save's exclusive card lock and return its result, or
   * return an acquisition error. Both borrows end before this call returns.
   * A NULL hook selects direct Pref access for an exclusively owned save. */
  h2_pal_result_t (*with_card_lock)(void* user, h2_buffet_card_work_fn work,
                                    void* context);
  /** Give lower-priority tasks time to run during sustained late frames. */
  bool cooperate_on_overrun;
  /** Optional synchronous diagnostic observer. A checkpoint is emitted before
   * the operation. The observer must be bounded and avoid I/O and blocking. */
  void (*checkpoint)(void* user, h2_buffet_stage_t stage);
  /** Release latched keys when changing between dining, its menu and the
   * ticket. */
  void (*release_keys)(void* user);
  /** Optional low-rate frame timing report on the game thread. */
  void (*report_profile)(void* user);
} h2_buffet_config_t;

/** Run the buffet on the calling thread. Borrow an initialized Runtime and
 * optional config for the call. The frame and touch surface are 800x480 RGB565
 * logical pixels at 2x density. Hosts aspect-fit presentation and map pointer
 * coordinates. Opens Display and Touch, releases both on every return, and
 * retains earned achievement keys in Pref when available. Returns OK on a host
 * close request or the first capability error. */
h2_pal_result_t h2_buffet_run(h2_runtime_t* runtime,
                              const h2_buffet_config_t* config);

#ifdef __cplusplus
}
#endif
#endif
