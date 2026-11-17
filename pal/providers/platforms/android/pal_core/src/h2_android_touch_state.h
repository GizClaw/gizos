#ifndef H2_ANDROID_TOUCH_STATE_H
#define H2_ANDROID_TOUCH_STATE_H
#include "hal/h2_pal_touch.h"
#include <stdbool.h>

/* Native input capture and polling are serialized by the platform mutex.
 * Adjacent MOVE events coalesce; DOWN/UP edges remain ordered between polls. */
enum { H2_ANDROID_TOUCH_QUEUE_CAPACITY = 64 };
typedef struct h2_android_touch_state {
  bool opened, pressed, pending_error;
  int32_t last_x, last_y;
  unsigned head, count;
  h2_pal_touch_event_t events[H2_ANDROID_TOUCH_QUEUE_CAPACITY];
} h2_android_touch_state_t;

static inline unsigned h2_android_touch_index(
    const h2_android_touch_state_t* state, unsigned offset) {
  return (state->head + offset) % H2_ANDROID_TOUCH_QUEUE_CAPACITY;
}
static inline void h2_android_touch_enqueue(h2_android_touch_state_t* state,
                                            h2_pal_touch_event_t event) {
  if (state->count && event.kind == H2_PAL_TOUCH_EVENT_MOVE) {
    unsigned tail = h2_android_touch_index(state, state->count - 1u);
    if (state->events[tail].kind == H2_PAL_TOUCH_EVENT_MOVE) {
      state->events[tail] = event;
      return;
    }
  }
  if (state->count == H2_ANDROID_TOUCH_QUEUE_CAPACITY) {
    unsigned remove = 0;
    while (remove < state->count &&
           state->events[h2_android_touch_index(state, remove)].kind !=
               H2_PAL_TOUCH_EVENT_MOVE)
      ++remove;
    if (remove < state->count) {
      for (unsigned i = remove; i + 1u < state->count; ++i)
        state->events[h2_android_touch_index(state, i)] =
            state->events[h2_android_touch_index(state, i + 1u)];
      --state->count;
    } else {
      /* No finite queue can retain an unbounded series of edges. Report loss
       * before resynchronizing, so a consumer cancels rather than connecting
       * two unrelated strokes. A currently held pointer restarts with DOWN. */
      state->head = state->count = 0;
      state->pending_error = true;
      if (event.kind == H2_PAL_TOUCH_EVENT_UP) return;
      event.kind = H2_PAL_TOUCH_EVENT_DOWN;
    }
  }
  state->events[h2_android_touch_index(state, state->count)] = event;
  ++state->count;
}
static inline int h2_android_touch_capture(h2_android_touch_state_t* state,
                                           int32_t width, int32_t height,
                                           int32_t x, int32_t y, bool down) {
  if (!state || width <= 0 || height <= 0) return H2_PAL_ERR_INVALID_ARG;
  if (!state->opened) return H2_PAL_ERR_INVALID_STATE;
  if (down) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
      state->head = state->count = 0;
      state->pending_error = true;
      state->pressed = false;
      return H2_PAL_ERR_IO;
    }
    if (state->pressed && x == state->last_x && y == state->last_y)
      return H2_PAL_OK;
    h2_android_touch_enqueue(
        state, (h2_pal_touch_event_t){state->pressed ? H2_PAL_TOUCH_EVENT_MOVE
                                                     : H2_PAL_TOUCH_EVENT_DOWN,
                                      x, y});
    state->pressed = true;
    state->last_x = x;
    state->last_y = y;
  } else if (state->pressed) {
    h2_android_touch_enqueue(
        state, (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, state->last_x,
                                      state->last_y});
    state->pressed = false;
  }
  return H2_PAL_OK;
}
static inline int h2_android_touch_poll(h2_android_touch_state_t* state,
                                        h2_pal_touch_event_t* out) {
  if (!state || !out) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_pal_touch_event_t){0};
  if (!state->opened) return H2_PAL_ERR_INVALID_STATE;
  if (state->pending_error) {
    state->pending_error = false;
    return H2_PAL_ERR_IO;
  }
  if (!state->count) return H2_PAL_ERR_WOULD_BLOCK;
  *out = state->events[state->head];
  state->head = (state->head + 1u) % H2_ANDROID_TOUCH_QUEUE_CAPACITY;
  --state->count;
  return H2_PAL_OK;
}
#endif
