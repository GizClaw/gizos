#ifndef H2_GIZCLAW_E2E_DEVKIT_STATE_H
#define H2_GIZCLAW_E2E_DEVKIT_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct h2_gizclaw_e2e_devkit_state {
  bool wifi_has_ip;
  bool clock_ready;
  bool runner_started;
  bool runner_complete;
  uint64_t next_summary_replay_ms;
} h2_gizclaw_e2e_devkit_state_t;

void h2_gizclaw_e2e_devkit_state_init(
    h2_gizclaw_e2e_devkit_state_t *state);

bool h2_gizclaw_e2e_devkit_state_set_prerequisites(
    h2_gizclaw_e2e_devkit_state_t *state, bool has_ip, bool clock_ready);

void h2_gizclaw_e2e_devkit_state_complete(
    h2_gizclaw_e2e_devkit_state_t *state, uint64_t now_ms,
    uint32_t replay_interval_ms);

bool h2_gizclaw_e2e_devkit_state_take_summary_replay(
    h2_gizclaw_e2e_devkit_state_t *state, uint64_t now_ms,
    uint32_t replay_interval_ms);

#endif
