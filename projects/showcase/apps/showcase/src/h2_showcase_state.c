#include "h2_showcase_state.h"

#include <stddef.h>
#include <string.h>

#define H2_SHOWCASE_TAP_TARGET 10u
#define H2_SHOWCASE_TAP_WINDOW_MS UINT64_C(5000)
#define H2_SHOWCASE_ADJACENT_TAP_MS UINT64_C(500)
#define H2_SHOWCASE_LONG_PRESS_MS UINT64_C(500)

static void reset_taps(h2_showcase_state_t *state) {
  state->first_tap_ms = 0u;
  state->previous_release_ms = 0u;
  state->tap_count = 0u;
}

void h2_showcase_state_init(h2_showcase_state_t *state) {
  if (state == NULL) {
    return;
  }
  memset(state, 0, sizeof(*state));
  state->mode = H2_SHOWCASE_MODE_IDLE;
}

void h2_showcase_state_button_down(h2_showcase_state_t *state,
                                   uint64_t timestamp_ms) {
  if (state == NULL || state->pressed ||
      state->mode == H2_SHOWCASE_MODE_CONSOLE) {
    return;
  }
  state->pressed = 1;
  state->press_started_ms = timestamp_ms;
}

void h2_showcase_state_button_up(h2_showcase_state_t *state,
                                 uint64_t timestamp_ms) {
  if (state == NULL) {
    return;
  }
  if (!state->pressed || state->mode == H2_SHOWCASE_MODE_CONSOLE) {
    state->pressed = 0;
    return;
  }
  state->pressed = 0;
  const uint64_t duration_ms = timestamp_ms >= state->press_started_ms
                                   ? timestamp_ms - state->press_started_ms
                                   : UINT64_MAX;
  if (duration_ms >= H2_SHOWCASE_LONG_PRESS_MS) {
    reset_taps(state);
    state->mode = H2_SHOWCASE_MODE_CONVERSATION;
    return;
  }

  if (state->tap_count == 0u || timestamp_ms < state->previous_release_ms ||
      timestamp_ms - state->previous_release_ms > H2_SHOWCASE_ADJACENT_TAP_MS ||
      timestamp_ms < state->first_tap_ms ||
      timestamp_ms - state->first_tap_ms > H2_SHOWCASE_TAP_WINDOW_MS) {
    state->tap_count = 1u;
    state->first_tap_ms = timestamp_ms;
  } else {
    ++state->tap_count;
  }
  state->previous_release_ms = timestamp_ms;
  state->mode = H2_SHOWCASE_MODE_IDLE;
  if (state->tap_count >= H2_SHOWCASE_TAP_TARGET) {
    reset_taps(state);
    state->mode = H2_SHOWCASE_MODE_CONSOLE;
  }
}

void h2_showcase_state_tick(h2_showcase_state_t *state, uint64_t timestamp_ms) {
  if (state == NULL || !state->pressed ||
      state->mode == H2_SHOWCASE_MODE_CONSOLE ||
      timestamp_ms < state->press_started_ms ||
      timestamp_ms - state->press_started_ms < H2_SHOWCASE_LONG_PRESS_MS) {
    return;
  }
  reset_taps(state);
  state->mode = H2_SHOWCASE_MODE_CONVERSATION;
}

void h2_showcase_state_close_console(h2_showcase_state_t *state) {
  if (state == NULL || state->mode != H2_SHOWCASE_MODE_CONSOLE) {
    return;
  }
  reset_taps(state);
  state->mode = H2_SHOWCASE_MODE_IDLE;
  state->pressed = 0;
}
