#include "h2_iperf_e2e_wifi.h"

#include <string.h>

h2_pal_result_t h2_iperf_e2e_select_wifi(
    const h2_pal_wifi_sta_api_t *sta,
    const h2_pal_wifi_settings_api_t *settings,
    h2_pal_wifi_sta_config_t *out_config,
    int *out_connected) {
    if (out_config != NULL) {
        memset(out_config, 0, sizeof(*out_config));
    }
    if (out_connected != NULL) {
        *out_connected = 0;
    }
    if (sta == NULL || settings == NULL || out_config == NULL ||
        out_connected == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_wifi_sta_status_t status = {0};
    int rc = h2_pal_wifi_sta_get_status(sta, &status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
        status.ip.ip4 != 0u) {
        *out_connected = 1;
        return H2_PAL_OK;
    }
    rc = h2_pal_wifi_settings_get_saved_sta_config(settings, out_config);
    if (rc != H2_PAL_OK) {
        memset(out_config, 0, sizeof(*out_config));
    }
    return rc;
}
