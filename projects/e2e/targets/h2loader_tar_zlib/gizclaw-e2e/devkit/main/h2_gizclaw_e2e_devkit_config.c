#include "h2_gizclaw_e2e_devkit_config.h"

#include <string.h>

const h2_gizclaw_e2e_devkit_config_t *h2_gizclaw_e2e_devkit_config(void) {
  static h2_gizclaw_e2e_devkit_config_t config = {
      .wifi_connect_timeout_ms = 10000u,
      .wifi_retry_interval_ms = 10000u,
      .summary_replay_interval_ms = 10000u,
  };
  const char *endpoint = h2_gizclaw_e2e_fixture_endpoint();
  const char *token = h2_gizclaw_e2e_fixture_token();
  config.server_endpoint = (h2_gizclaw_str_t){endpoint, strlen(endpoint)};
  config.registration_token = (h2_gizclaw_str_t){token, strlen(token)};
  return &config;
}
