#include "h2_wifi_sta.h"

#include <string.h>

static int remaining_budget(const h2_wifi_sta_dependencies_t *deps, uint64_t started,
                            uint32_t budget, uint32_t *remaining) {
    uint64_t now = 0;
    int rc = h2_pal_time_get_monotonic_ms(deps->time, &now);
    if (rc != H2_PAL_OK)
        return rc;
    if (now < started || now - started >= budget)
        return H2_PAL_ERR_TIMEOUT;
    *remaining = budget - (uint32_t)(now - started);
    return H2_PAL_OK;
}

int h2_wifi_sta_connect_and_save(const h2_wifi_sta_dependencies_t *deps,
                            const h2_pal_wifi_sta_config_t *config,
                            uint32_t timeout_ms) {
    int validation = h2_pal_wifi_settings_validate_sta_config(config);
    if (validation != H2_PAL_OK)
        return validation;
    if (!deps || !deps->sta || !deps->time || timeout_ms == 0u)
        return H2_PAL_ERR_INVALID_ARG;
    /* Refuse before changing networks if durable storage is unavailable. */
    const h2_pal_wifi_settings_api_t *settings = deps->settings;
    if (!settings || !settings->vtable ||
        !settings->vtable->set_saved_sta_config)
        return H2_PAL_ERR_UNSUPPORTED;
    h2_pal_wifi_sta_config_t saved = {0};
    int rc = h2_pal_wifi_settings_get_saved_sta_config(settings, &saved);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND)
        return rc;
    memset(&saved, 0, sizeof(saved));
    uint64_t started = 0;
    rc = h2_pal_time_get_monotonic_ms(deps->time, &started);
    if (rc != H2_PAL_OK)
        return rc;
    uint32_t budget = timeout_ms;
    /* Re-authenticate even for the same SSID: cached association is not
     * evidence that a newly supplied password works. */
    h2_pal_wifi_sta_status_t previous = {0};
    rc = h2_pal_wifi_sta_get_status(deps->sta, &previous);
    if (rc != H2_PAL_OK)
        return rc;
    /* Cancel an in-flight asynchronous attempt too: it must not complete
     * between the snapshot and connect and validate stale credentials. */
    rc = h2_pal_wifi_sta_disconnect(deps->sta);
    if (rc != H2_PAL_OK)
        return rc;
    uint32_t remaining = 0;
    rc = remaining_budget(deps, started, budget, &remaining);
    if (rc != H2_PAL_OK)
        return rc;
    rc = h2_pal_wifi_sta_connect(deps->sta, config, remaining);
    while (rc == H2_PAL_OK) {
        h2_pal_wifi_sta_status_t status = {0};
        rc = h2_pal_wifi_sta_get_status(deps->sta, &status);
        if (rc != H2_PAL_OK)
            break;
        rc = remaining_budget(deps, started, budget, &remaining);
        if (rc != H2_PAL_OK)
            break;
        if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid && status.ip.ip4 != 0u &&
            status.ssid_len == config->ssid_len &&
            memcmp(status.ssid, config->ssid, config->ssid_len) == 0 &&
            (!config->bssid_set || (status.bssid_set &&
             memcmp(status.bssid, config->bssid, sizeof(config->bssid)) == 0))) {
            return h2_pal_wifi_settings_set_saved_sta_config(settings, config);
        }
        if (status.state == H2_PAL_WIFI_STA_STATE_FAILED ||
            status.state == H2_PAL_WIFI_STA_STATE_DISCONNECTED)
            return H2_PAL_ERR_UNAVAILABLE;
        rc = h2_pal_time_sleep_ms(deps->time, remaining < 20u ? remaining : 20u);
    }
    return rc;
}
