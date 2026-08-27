#ifndef H2_GIZCLAW_E2E_DEVKIT_STATE_H
#define H2_GIZCLAW_E2E_DEVKIT_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"

typedef struct h2_gizclaw_e2e_devkit_state {
  bool wifi_has_ip;
  bool clock_ready;
  bool runner_started;
  bool runner_complete;
  uint64_t next_summary_replay_ms;
} h2_gizclaw_e2e_devkit_state_t;

typedef enum h2_gizclaw_e2e_devkit_wifi_outcome {
  H2_GIZCLAW_E2E_DEVKIT_WIFI_WAIT = 0,
  H2_GIZCLAW_E2E_DEVKIT_WIFI_CONNECTED,
  H2_GIZCLAW_E2E_DEVKIT_WIFI_NO_SAVED_CONFIG,
  H2_GIZCLAW_E2E_DEVKIT_WIFI_RETRY,
} h2_gizclaw_e2e_devkit_wifi_outcome_t;

typedef struct h2_gizclaw_e2e_devkit_wifi_result {
  h2_gizclaw_e2e_devkit_wifi_outcome_t outcome;
  int rc;
} h2_gizclaw_e2e_devkit_wifi_result_t;

int h2_gizclaw_e2e_devkit_wifi_step(
    const h2_pal_wifi_sta_api_t *wifi_sta,
    const h2_pal_wifi_settings_api_t *wifi_settings,
    uint32_t connect_timeout_ms,
    h2_gizclaw_e2e_devkit_wifi_result_t *out_result);

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
