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
} h2_buffet_action_t;

/** Optional host controls. Axes are held directions in -1..1. Actions are
 * rising edges only. An inactive host freezes the meal and clears movement. */
typedef struct h2_buffet_input {
  int dx, dy;
  unsigned actions;
  bool inactive, close;
} h2_buffet_input_t;

typedef struct h2_buffet_config {
  void* user;
  void (*poll)(void* user, h2_buffet_input_t* input);
  bool keyboard;
  bool reduced_motion;
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
