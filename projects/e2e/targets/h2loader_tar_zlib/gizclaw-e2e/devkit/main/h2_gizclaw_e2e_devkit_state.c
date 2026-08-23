#include "h2_gizclaw_e2e_devkit_state.h"

#include <stddef.h>

static bool deadline_expired(uint64_t now_ms, uint64_t deadline_ms) {
  return (now_ms - deadline_ms) < (UINT64_C(1) << 63);
}

void h2_gizclaw_e2e_devkit_state_init(
    h2_gizclaw_e2e_devkit_state_t *state) {
  if (state == NULL) {
    return;
  }
  *state = (h2_gizclaw_e2e_devkit_state_t){0};
}

bool h2_gizclaw_e2e_devkit_state_set_prerequisites(
    h2_gizclaw_e2e_devkit_state_t *state, bool has_ip, bool clock_ready) {
  if (state == NULL) {
    return false;
  }
  state->wifi_has_ip = has_ip;
  state->clock_ready = state->clock_ready || clock_ready;
  if (!has_ip || !state->clock_ready || state->runner_started) {
    return false;
  }
  state->runner_started = true;
  return true;
}

void h2_gizclaw_e2e_devkit_state_complete(
    h2_gizclaw_e2e_devkit_state_t *state, uint64_t now_ms,
    uint32_t replay_interval_ms) {
  if (state == NULL || !state->runner_started || state->runner_complete) {
    return;
  }
  state->runner_complete = true;
  state->next_summary_replay_ms = now_ms + (uint64_t)replay_interval_ms;
}

bool h2_gizclaw_e2e_devkit_state_take_summary_replay(
    h2_gizclaw_e2e_devkit_state_t *state, uint64_t now_ms,
    uint32_t replay_interval_ms) {
  if (state == NULL || !state->runner_complete || replay_interval_ms == 0u ||
      !deadline_expired(now_ms, state->next_summary_replay_ms)) {
    return false;
  }
  state->next_summary_replay_ms = now_ms + (uint64_t)replay_interval_ms;
  return true;
}
