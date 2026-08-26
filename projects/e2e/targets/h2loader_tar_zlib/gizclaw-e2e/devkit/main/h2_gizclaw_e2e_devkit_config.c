#include "h2_gizclaw_e2e_devkit_config.h"

#define H2_GIZCLAW_E2E_DEVKIT_ENDPOINT "edge-bj-01.e2e.gizclaw.com:9821"
#define H2_GIZCLAW_E2E_DEVKIT_REGISTRATION_TOKEN "deploy-default"

const h2_gizclaw_e2e_devkit_config_t *h2_gizclaw_e2e_devkit_config(void) {
  static const h2_gizclaw_e2e_devkit_config_t config = {
      .server_endpoint = {H2_GIZCLAW_E2E_DEVKIT_ENDPOINT,
                          sizeof(H2_GIZCLAW_E2E_DEVKIT_ENDPOINT) - 1u},
      .registration_token = {
          H2_GIZCLAW_E2E_DEVKIT_REGISTRATION_TOKEN,
          sizeof(H2_GIZCLAW_E2E_DEVKIT_REGISTRATION_TOKEN) - 1u},
      .wifi_connect_timeout_ms = 10000u,
      .wifi_retry_interval_ms = 10000u,
      .summary_replay_interval_ms = 10000u,
  };
  return &config;
}
