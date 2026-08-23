#ifndef H2_SHOWCASE_STATE_H
#define H2_SHOWCASE_STATE_H

#include <stdint.h>

typedef enum h2_showcase_mode {
  H2_SHOWCASE_MODE_IDLE = 0,
  H2_SHOWCASE_MODE_CONVERSATION,
  H2_SHOWCASE_MODE_CONSOLE,
} h2_showcase_mode_t;

typedef struct h2_showcase_state {
  h2_showcase_mode_t mode;
  uint64_t press_started_ms;
  uint64_t first_tap_ms;
  uint64_t previous_release_ms;
  uint8_t tap_count;
  int pressed;
} h2_showcase_state_t;

void h2_showcase_state_init(h2_showcase_state_t *state);
void h2_showcase_state_button_down(h2_showcase_state_t *state,
                                   uint64_t timestamp_ms);
void h2_showcase_state_button_up(h2_showcase_state_t *state,
                                 uint64_t timestamp_ms);
void h2_showcase_state_tick(h2_showcase_state_t *state, uint64_t timestamp_ms);
void h2_showcase_state_close_console(h2_showcase_state_t *state);

#endif
