#ifndef H2_IPERF_E2E_WIFI_H
#define H2_IPERF_E2E_WIFI_H

#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Selects an existing IPv4 connection or a saved profile for launcher setup.
 *
 * All arguments are required and borrowed for this call only. On H2_PAL_OK,
 * out_connected is one when the station already has GOT_IP and a valid nonzero
 * IPv4 address; otherwise out_config contains the saved profile. The launcher
 * owns connecting, waiting and any retries before starting its workload.
 *
 * This call never connects or changes saved credentials. Missing saved settings
 * return H2_PAL_ERR_NOT_FOUND; other PAL errors are preserved. Both outputs are
 * cleared on failure. This prerequisite must not restart a running matrix.
 */
h2_pal_result_t h2_iperf_e2e_select_wifi(
    const h2_pal_wifi_sta_api_t *sta,
    const h2_pal_wifi_settings_api_t *settings,
    h2_pal_wifi_sta_config_t *out_config,
    int *out_connected);

#ifdef __cplusplus
}
#endif

#endif
