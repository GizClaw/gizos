#include "h2_esp_platform_core.h"

#include <string.h>

int h2_esp_platform_wifi_ensure_started(void) {
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_unsupported_get_status(
    h2_pal_wifi_sta_t *sta,
    h2_pal_wifi_sta_status_t *out_status) {
    (void)sta;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->state = H2_PAL_WIFI_STA_STATE_UNKNOWN;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_unsupported_scan(
    h2_pal_wifi_sta_t *sta,
    const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result,
    void *user,
    uint32_t timeout_ms) {
    (void)sta;
    (void)request;
    (void)on_result;
    (void)user;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_unsupported_connect(
    h2_pal_wifi_sta_t *sta,
    const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms) {
    (void)sta;
    (void)config;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_unsupported_disconnect(h2_pal_wifi_sta_t *sta) {
    (void)sta;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_unsupported_get_mac(h2_pal_wifi_sta_t *sta, uint8_t out_mac[6]) {
    (void)sta;
    if (out_mac == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_mac, 0, 6u);
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_ap_unsupported_start(
    h2_pal_wifi_ap_t *ap,
    const h2_pal_wifi_ap_config_t *config,
    uint32_t timeout_ms) {
    (void)ap;
    (void)config;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_ap_unsupported_stop(h2_pal_wifi_ap_t *ap, uint32_t timeout_ms) {
    (void)ap;
    (void)timeout_ms;
    return H2_PAL_OK;
}

static int h2_esp_wifi_ap_unsupported_get_status(
    h2_pal_wifi_ap_t *ap,
    h2_pal_wifi_ap_status_t *out_status) {
    (void)ap;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->state = H2_PAL_WIFI_AP_STATE_STOPPED;
    return H2_PAL_OK;
}

static int h2_esp_wifi_ap_unsupported_get_clients(
    h2_pal_wifi_ap_t *ap,
    h2_pal_wifi_ap_client_t *out_clients,
    size_t max_clients,
    size_t *out_count) {
    (void)ap;
    (void)out_clients;
    (void)max_clients;
    if (out_count == NULL || (max_clients > 0u && out_clients == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = 0u;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_ap_unsupported_get_mac(h2_pal_wifi_ap_t *ap, uint8_t out_mac[6]) {
    (void)ap;
    if (out_mac == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_mac, 0, 6u);
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_wifi_sta_vtable_t s_h2_esp_wifi_sta_vtable = {
    .get_status = (h2_pal_wifi_sta_get_status_fn)h2_esp_wifi_unsupported_get_status,
    .scan = (h2_pal_wifi_sta_scan_fn)h2_esp_wifi_unsupported_scan,
    .connect = (h2_pal_wifi_sta_connect_fn)h2_esp_wifi_unsupported_connect,
    .disconnect = (h2_pal_wifi_sta_disconnect_fn)h2_esp_wifi_unsupported_disconnect,
    .get_mac = (h2_pal_wifi_sta_get_mac_fn)h2_esp_wifi_unsupported_get_mac,
};

static h2_pal_wifi_sta_t s_h2_esp_wifi_sta = {
    .user = &s_h2_esp_wifi_sta,
    .vtable = &s_h2_esp_wifi_sta_vtable,
};

static const h2_pal_wifi_ap_vtable_t s_h2_esp_wifi_ap_vtable = {
    .start = (h2_pal_wifi_ap_start_fn)h2_esp_wifi_ap_unsupported_start,
    .stop = (h2_pal_wifi_ap_stop_fn)h2_esp_wifi_ap_unsupported_stop,
    .get_status = (h2_pal_wifi_ap_get_status_fn)h2_esp_wifi_ap_unsupported_get_status,
    .get_clients = (h2_pal_wifi_ap_get_clients_fn)h2_esp_wifi_ap_unsupported_get_clients,
    .get_mac = (h2_pal_wifi_ap_get_mac_fn)h2_esp_wifi_ap_unsupported_get_mac,
};

static h2_pal_wifi_ap_t s_h2_esp_wifi_ap = {
    .user = &s_h2_esp_wifi_ap,
    .vtable = &s_h2_esp_wifi_ap_vtable,
};

h2_pal_wifi_sta_t *h2_esp_platform_wifi_sta(void) {
    return &s_h2_esp_wifi_sta;
}

h2_pal_wifi_ap_t *h2_esp_platform_wifi_ap(void) {
    return &s_h2_esp_wifi_ap;
}

int h2_esp_platform_wifi_connect_saved(uint32_t timeout_ms) {
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}
