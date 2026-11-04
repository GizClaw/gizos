#ifndef H2_GIZCLAW_E2E_WS175C_CONFIG_H
#define H2_GIZCLAW_E2E_WS175C_CONFIG_H

#include "h2_gizclaw_e2e.h"

#include <stdint.h>

typedef struct h2_gizclaw_e2e_ws175c_config {
  h2_gizclaw_str_t server_endpoint;
  h2_gizclaw_str_t registration_token;
  uint32_t wifi_connect_timeout_ms;
  uint32_t wifi_retry_interval_ms;
  uint32_t summary_replay_interval_ms;
} h2_gizclaw_e2e_ws175c_config_t;

const h2_gizclaw_e2e_ws175c_config_t* h2_gizclaw_e2e_ws175c_config(void);

#endif
