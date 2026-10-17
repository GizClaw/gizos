#ifndef H2_ESP_E2E_NETWORK_PROVISION_H
#define H2_ESP_E2E_NETWORK_PROVISION_H

#include "result_console.h"
#include "h2/pal/hal/h2_pal_wifi.h"

/** Optional public fixture mapping for native consumers that opt in through
 * H2_E2E_WIFI_FIXTURE. The generic provisioner below takes any borrowed PAL
 * configuration and does not own this caller-selected network policy. */
static inline const h2_pal_wifi_sta_config_t *h2_esp_e2e_public_wifi_fixture(void) {
#if defined(H2_E2E_WIFI_FIXTURE_HAIVIVI_MFG)
    static const h2_pal_wifi_sta_config_t fixture = {
        .ssid = "HAIVIVI-MFG", .ssid_len = sizeof("HAIVIVI-MFG") - 1u,
        .password = "!haivivi", .password_len = sizeof("!haivivi") - 1u,
    };
    return &fixture;
#else
    return NULL;
#endif
}

static inline int h2_esp_e2e_wifi_observe(h2_esp_e2e_console_t *console,
    const char *phase, int saved, int sta_state, int ap_state, int ap_rc, int temporary, int rc) {
    const char *capability = ap_rc == H2_PAL_ERR_UNSUPPORTED ? "unsupported" :
        ap_rc != H2_PAL_OK ? "error" : ap_state < 0 ? "unobserved" : "supported";
    char line[256];
    int length = snprintf(line, sizeof(line),
        "H2_E2E_WIFI_PROVISION phase=%s saved=%d pre_sta_state=%d pre_ap_state=%d ap_capability=%s ap_rc=%d temporary=%d rc=%d",
        phase, saved, sta_state, ap_state, capability, ap_rc, temporary, rc);
    if (length < 0 || (size_t)length >= sizeof(line)) return H2_PAL_ERR_TRUNCATED;
    int output = h2_esp_e2e_console_record(console, line);
    return output != H2_PAL_OK ? output : rc;
}

/**
 * @brief Optional native E2E setup, called once by the worker before cases.
 * All APIs/configuration are borrowed until return. NULL or empty credentials
 * disable setup without reading a provider or writing an observation. A ready
 * or in-flight STA and saved credentials retain the caller's existing path.
 * Only known disconnected STA and actual Saved=0 allow a nonpersistent PAL
 * connect with the caller's association budget. Supported AP must be stopped;
 * allow_sta_only explicitly accepts UNSUPPORTED without claiming AP stopped.
 * Success does not imply an IP; the original suite must still wait for one.
 * Apart from opted-in AP UNSUPPORTED, API/output errors must prevent cases and
 * confirmation and are returned to the caller.
 * Call from task context and serialize other network mutations until return.
 * No Settings set/clear, retry, disconnect, extra wait or credential log occurs.
 */
static inline int h2_esp_e2e_provision_wifi(
    const h2_pal_wifi_sta_api_t *sta, const h2_pal_wifi_ap_api_t *ap,
    const h2_pal_wifi_settings_api_t *settings,
    const h2_pal_wifi_sta_config_t *fixture, uint32_t timeout_ms, bool allow_sta_only,
    h2_esp_e2e_console_t *console) {
    if (fixture == NULL || (fixture->ssid_len == 0u && fixture->password_len == 0u))
        return H2_PAL_OK;
    int rc = h2_pal_wifi_settings_validate_sta_config(fixture);
    if (rc == H2_PAL_OK && timeout_ms == 0u) rc = H2_PAL_ERR_INVALID_ARG;
    if (rc != H2_PAL_OK)
        return h2_esp_e2e_wifi_observe(console, "config", -1, -1, -1, H2_PAL_OK, 0, rc);
    h2_pal_wifi_sta_status_t current = {0};
    rc = h2_pal_wifi_sta_get_status(sta, &current);
    if (rc != H2_PAL_OK)
        return h2_esp_e2e_wifi_observe(console, "sta", -1, -1, -1, H2_PAL_OK, 0, rc);
    if (current.state == H2_PAL_WIFI_STA_STATE_SCANNING ||
        current.state == H2_PAL_WIFI_STA_STATE_CONNECTING ||
        current.state == H2_PAL_WIFI_STA_STATE_CONNECTED ||
        current.state == H2_PAL_WIFI_STA_STATE_GOT_IP)
        return h2_esp_e2e_wifi_observe(console, "existing", -1, current.state, -1, H2_PAL_OK, 0, H2_PAL_OK);
    if ((current.state != H2_PAL_WIFI_STA_STATE_IDLE &&
         current.state != H2_PAL_WIFI_STA_STATE_DISCONNECTED &&
         current.state != H2_PAL_WIFI_STA_STATE_FAILED) || current.ip_valid || current.ip.ip6_valid)
        return h2_esp_e2e_wifi_observe(console, "sta", -1, current.state, -1, H2_PAL_OK, 0, H2_PAL_ERR_INVALID_STATE);
    int saved = -1;
    rc = h2_pal_wifi_settings_has_saved_sta_config(settings, &saved);
    if (rc == H2_PAL_OK && saved != 0 && saved != 1) rc = H2_PAL_ERR_FORMAT;
    if (rc != H2_PAL_OK)
        return h2_esp_e2e_wifi_observe(console, "settings", -1, current.state, -1, H2_PAL_OK, 0, rc);
    if (saved)
        return h2_esp_e2e_wifi_observe(console, "saved", saved, current.state, -1, H2_PAL_OK, 0, H2_PAL_OK);
    h2_pal_wifi_ap_status_t access_point = {0};
    int ap_rc = h2_pal_wifi_ap_get_status(ap, &access_point);
    int ap_state = ap_rc == H2_PAL_OK ? (int)access_point.state : -1;
    rc = ap_rc == H2_PAL_ERR_UNSUPPORTED && allow_sta_only ? H2_PAL_OK : ap_rc;
    if (ap_rc == H2_PAL_OK && access_point.state != H2_PAL_WIFI_AP_STATE_STOPPED)
        rc = access_point.state == H2_PAL_WIFI_AP_STATE_STARTING ||
             access_point.state == H2_PAL_WIFI_AP_STATE_STARTED ||
             access_point.state == H2_PAL_WIFI_AP_STATE_STOPPING ? H2_PAL_ERR_BUSY : H2_PAL_ERR_INVALID_STATE;
    if (rc != H2_PAL_OK)
        return h2_esp_e2e_wifi_observe(console, "ap", saved, current.state, ap_state, ap_rc, 0, rc);
    rc = h2_esp_e2e_wifi_observe(console, "eligible", saved, current.state, ap_state, ap_rc, 0, H2_PAL_OK);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_pal_wifi_sta_connect(sta, fixture, timeout_ms);
    return h2_esp_e2e_wifi_observe(console, "connect", saved, current.state, ap_state, ap_rc, 1, rc);
}

#endif
