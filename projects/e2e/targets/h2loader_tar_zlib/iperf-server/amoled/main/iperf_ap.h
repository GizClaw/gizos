#ifndef H2_IPERF_AMOLED_AP_H
#define H2_IPERF_AMOLED_AP_H
#include "esp_netif.h"
#include "h2_iperf_server_app.h"
#include "lwip/raw.h"

typedef struct h2_iperf_amoled_ap {
  h2_runtime_t *runtime;
  esp_netif_t *ap;
  struct raw_pcb *advertiser;
  bool active;
  int callback_result;
} h2_iperf_amoled_ap_t;

int h2_iperf_amoled_ap_start(void *user, h2_iperf_server_app_mode_t mode,
                             h2_iperf_server_app_network_t *out_network);
int h2_iperf_amoled_ap_stop(void *user);
#endif
