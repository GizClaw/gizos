#ifndef H2_ANDROID_TOUCH_STATE_H
#define H2_ANDROID_TOUCH_STATE_H
#include "hal/h2_pal_touch.h"
#include <stdbool.h>

typedef struct h2_android_touch_state {
  bool opened, pressed;
  int32_t last_x, last_y;
} h2_android_touch_state_t;

/* Called with the platform mutex held. Empty/invalid snapshots preserve the
 * last valid edge. Release uses its last valid coordinate, not an OS sentinel.
 */
static inline int h2_android_touch_snapshot(h2_android_touch_state_t* state,
                                            int32_t width, int32_t height,
                                            int32_t x, int32_t y, bool down,
                                            h2_pal_touch_event_t* out) {
  if (!state || !out || width <= 0 || height <= 0)
    return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_pal_touch_event_t){0};
  if (!state->opened) return H2_PAL_ERR_INVALID_STATE;
  if (down) {
    if (x < 0 || y < 0 || x >= width || y >= height) return H2_PAL_ERR_IO;
    if (state->pressed && x == state->last_x && y == state->last_y)
      return H2_PAL_ERR_WOULD_BLOCK;
    *out = (h2_pal_touch_event_t){
        state->pressed ? H2_PAL_TOUCH_EVENT_MOVE : H2_PAL_TOUCH_EVENT_DOWN, x,
        y};
    state->pressed = true;
    state->last_x = x;
    state->last_y = y;
    return H2_PAL_OK;
  }
  if (!state->pressed) return H2_PAL_ERR_WOULD_BLOCK;
  *out = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, state->last_x,
                                state->last_y};
  state->pressed = false;
  return H2_PAL_OK;
}
#endif
