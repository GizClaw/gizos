#include "h2_gizclaw_e2e_amoled_state.h"

#include <stddef.h>

static bool deadline_expired(uint64_t now_ms, uint64_t deadline_ms) {
  return (now_ms - deadline_ms) < (UINT64_C(1) << 63);
}

int h2_gizclaw_e2e_amoled_wifi_step(
    const h2_pal_wifi_sta_api_t *wifi_sta,
    const h2_pal_wifi_settings_api_t *wifi_settings,
    uint32_t connect_timeout_ms,
    h2_gizclaw_e2e_amoled_wifi_result_t *out_result) {
  h2_pal_wifi_sta_config_t saved = {0};
  h2_pal_wifi_sta_status_t status = {0};
  if (wifi_sta == NULL || wifi_settings == NULL || out_result == NULL ||
      connect_timeout_ms == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_result = (h2_gizclaw_e2e_amoled_wifi_result_t){
      .outcome = H2_GIZCLAW_E2E_AMOLED_WIFI_WAIT,
      .rc = H2_PAL_OK,
  };
  int rc = h2_pal_wifi_sta_get_status(wifi_sta, &status);
  if (rc == H2_PAL_OK &&
      ((status.state == H2_PAL_WIFI_STA_STATE_GOT_IP &&
        status.ip_valid != 0u) ||
       status.state == H2_PAL_WIFI_STA_STATE_CONNECTING ||
       status.state == H2_PAL_WIFI_STA_STATE_CONNECTED)) {
    return H2_PAL_OK;
  }
  rc = h2_pal_wifi_settings_get_saved_sta_config(wifi_settings, &saved);
  if (rc == H2_PAL_OK) {
    rc = h2_pal_wifi_settings_validate_sta_config(&saved);
  }
  if (rc != H2_PAL_OK) {
    out_result->outcome = H2_GIZCLAW_E2E_AMOLED_WIFI_NO_SAVED_CONFIG;
    out_result->rc = rc;
    return H2_PAL_OK;
  }
  rc = h2_pal_wifi_sta_connect(wifi_sta, &saved, connect_timeout_ms);
  out_result->outcome = rc == H2_PAL_OK
      ? H2_GIZCLAW_E2E_AMOLED_WIFI_CONNECTED
      : H2_GIZCLAW_E2E_AMOLED_WIFI_RETRY;
  out_result->rc = rc;
  return H2_PAL_OK;
}

void h2_gizclaw_e2e_amoled_state_init(
    h2_gizclaw_e2e_amoled_state_t *state) {
  if (state == NULL) {
    return;
  }
  *state = (h2_gizclaw_e2e_amoled_state_t){0};
}

bool h2_gizclaw_e2e_amoled_state_set_prerequisites(
    h2_gizclaw_e2e_amoled_state_t *state, bool has_ip, bool clock_ready) {
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

void h2_gizclaw_e2e_amoled_state_complete(
    h2_gizclaw_e2e_amoled_state_t *state, uint64_t now_ms,
    uint32_t replay_interval_ms) {
  if (state == NULL || !state->runner_started || state->runner_complete) {
    return;
  }
  state->runner_complete = true;
  state->next_summary_replay_ms = now_ms + (uint64_t)replay_interval_ms;
}

bool h2_gizclaw_e2e_amoled_state_take_summary_replay(
    h2_gizclaw_e2e_amoled_state_t *state, uint64_t now_ms,
    uint32_t replay_interval_ms) {
  if (state == NULL || !state->runner_complete || replay_interval_ms == 0u ||
      !deadline_expired(now_ms, state->next_summary_replay_ms)) {
    return false;
  }
  state->next_summary_replay_ms = now_ms + (uint64_t)replay_interval_ms;
  return true;
}
