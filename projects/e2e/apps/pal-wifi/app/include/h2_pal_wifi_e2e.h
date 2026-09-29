#ifndef H2_PAL_WIFI_E2E_H
#define H2_PAL_WIFI_E2E_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*h2_wifi_case_result_fn)(void *user, const char *id, int rc,
                                      uint64_t elapsed_ms);

typedef struct h2_wifi_e2e_config {
    /* Synthetic fixture credentials only. Production credentials are read from
     * Settings and never passed to a reporter. Caller owns all input storage. */
    h2_pal_wifi_sta_config_t fixture;
    h2_pal_wifi_ap_config_t ap;
    uint32_t operation_timeout_ms;
    uint32_t client_timeout_ms;
    h2_wifi_case_result_fn case_result;
    void *user;
} h2_wifi_e2e_config_t;

typedef struct h2_wifi_e2e_result {
    unsigned passed, failed, blocked;
    int cleanup_rc;
    int saved_restored;
    int network_restored;
    unsigned retained;
    unsigned last_error_line;
    unsigned sta_connecting, sta_connected, sta_got_ip, sta_disconnected;
    unsigned ap_started, ap_stopped, client_joined, client_left;
    unsigned lease_granted, lease_released, route_changed, invalid_events;
    uint8_t client_mac[6];
    uint32_t client_ip4;
} h2_wifi_e2e_result_t;

/* Never owns/deinitializes Runtime. The launcher must make a durable backup of
 * original Settings before invoking, to recover even after power interruption.
 * Every case runs; terminal restoration always runs and is qualification. */
int h2_wifi_e2e_run(h2_runtime_t *runtime, const h2_wifi_e2e_config_t *config,
                    h2_wifi_e2e_result_t *out_result);
int h2_wifi_config_equal(const h2_pal_wifi_sta_config_t *a,
                          const h2_pal_wifi_sta_config_t *b);

/* Read-only host Netif and explicit unsupported capability qualification.
 * Returns no physical WLAN qualification. */
int h2_wifi_host_contract(const h2_pal_wifi_sta_api_t *sta,
                           const h2_pal_wifi_ap_api_t *ap,
                           const h2_pal_wifi_settings_api_t *settings,
                           const h2_pal_netif_api_t *netif,
                           int host_kind, unsigned *out_netifs);

#ifdef __cplusplus
}
#endif

#endif
