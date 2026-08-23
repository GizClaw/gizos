#include "h2_showcase_state.h"

#include <assert.h>

static void tap(h2_showcase_state_t *state, uint64_t down_ms, uint64_t up_ms) {
  h2_showcase_state_button_down(state, down_ms);
  h2_showcase_state_button_up(state, up_ms);
}

int main(void) {
  h2_showcase_state_t state;
  h2_showcase_state_init(&state);
  for (uint64_t index = 0; index < 9u; ++index) {
    tap(&state, index * 300u, index * 300u + 50u);
  }
  assert(state.mode == H2_SHOWCASE_MODE_IDLE);
  tap(&state, 2700u, 2750u);
  assert(state.mode == H2_SHOWCASE_MODE_CONSOLE);

  h2_showcase_state_close_console(&state);
  tap(&state, 2800u, 2810u);
  assert(state.mode == H2_SHOWCASE_MODE_IDLE);
  assert(state.tap_count == 1u);

  h2_showcase_state_init(&state);
  h2_showcase_state_button_down(&state, 100u);
  h2_showcase_state_tick(&state, 599u);
  assert(state.mode == H2_SHOWCASE_MODE_IDLE);
  h2_showcase_state_tick(&state, 600u);
  assert(state.mode == H2_SHOWCASE_MODE_CONVERSATION);
  h2_showcase_state_button_up(&state, 600u);
  assert(state.mode == H2_SHOWCASE_MODE_CONVERSATION);
  assert(state.tap_count == 0u);

  h2_showcase_state_init(&state);
  tap(&state, 0u, 20u);
  tap(&state, 600u, 620u);
  assert(state.tap_count == 1u);
  for (uint64_t index = 0; index < 9u; ++index) {
    tap(&state, 800u + index * 300u, 820u + index * 300u);
  }
  assert(state.mode == H2_SHOWCASE_MODE_CONSOLE);
  return 0;
}
