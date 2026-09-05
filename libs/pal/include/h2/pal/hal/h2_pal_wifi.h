#ifndef H2_PAL_WIFI_H
#define H2_PAL_WIFI_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_WIFI_SCAN_MAX_RESULTS 16
#define H2_PAL_WIFI_AP_MAX_CLIENTS 8

typedef enum h2_pal_wifi_interface {
    H2_PAL_WIFI_INTERFACE_STA = 1,
    H2_PAL_WIFI_INTERFACE_AP = 2,
} h2_pal_wifi_interface_t;

typedef enum h2_pal_wifi_security {
    H2_PAL_WIFI_SECURITY_UNKNOWN = 0,
    H2_PAL_WIFI_SECURITY_OPEN = 1,
    H2_PAL_WIFI_SECURITY_WEP = 2,
    H2_PAL_WIFI_SECURITY_WPA = 3,
    H2_PAL_WIFI_SECURITY_WPA2 = 4,
    H2_PAL_WIFI_SECURITY_WPA3 = 5,
    H2_PAL_WIFI_SECURITY_WPA_WPA2 = 6,
    H2_PAL_WIFI_SECURITY_WPA2_WPA3 = 7,
    H2_PAL_WIFI_SECURITY_ENTERPRISE = 8,
} h2_pal_wifi_security_t;

typedef enum h2_pal_wifi_sta_state {
    H2_PAL_WIFI_STA_STATE_UNKNOWN = 0,
    H2_PAL_WIFI_STA_STATE_IDLE = 1,
    H2_PAL_WIFI_STA_STATE_SCANNING = 2,
    H2_PAL_WIFI_STA_STATE_CONNECTING = 3,
    H2_PAL_WIFI_STA_STATE_CONNECTED = 4,
    H2_PAL_WIFI_STA_STATE_GOT_IP = 5,
    H2_PAL_WIFI_STA_STATE_DISCONNECTED = 6,
    H2_PAL_WIFI_STA_STATE_FAILED = 7,
} h2_pal_wifi_sta_state_t;

/**
 * Station power-save policy.
 *
 * NONE keeps the radio awake for the lowest latency, MIN_MODEM sleeps until
 * the next DTIM beacon, MAX_MODEM sleeps across the listen interval.
 * Providers report H2_PAL_ERR_UNSUPPORTED when a mode cannot be selected.
 */
typedef enum h2_pal_wifi_power_save {
    H2_PAL_WIFI_POWER_SAVE_NONE = 0,
    H2_PAL_WIFI_POWER_SAVE_MIN_MODEM = 1,
    H2_PAL_WIFI_POWER_SAVE_MAX_MODEM = 2,
} h2_pal_wifi_power_save_t;

typedef enum h2_pal_wifi_ap_state {
    H2_PAL_WIFI_AP_STATE_UNKNOWN = 0,
    H2_PAL_WIFI_AP_STATE_STOPPED = 1,
    H2_PAL_WIFI_AP_STATE_STARTING = 2,
    H2_PAL_WIFI_AP_STATE_STARTED = 3,
    H2_PAL_WIFI_AP_STATE_STOPPING = 4,
} h2_pal_wifi_ap_state_t;

typedef struct h2_pal_wifi_ip_info {
    /* IPv4 octets use network significance: a.b.c.d is 0xaabbccdd. */
    uint32_t ip4;
    uint32_t netmask4;
    uint32_t gateway4;
} h2_pal_wifi_ip_info_t;

static inline void h2_pal_wifi_ip4_to_bytes(uint32_t ip4, uint8_t out[4]) {
    if (out == NULL) {
        return;
    }
    out[0] = (uint8_t)(ip4 >> 24);
    out[1] = (uint8_t)(ip4 >> 16);
    out[2] = (uint8_t)(ip4 >> 8);
    out[3] = (uint8_t)ip4;
}

typedef struct h2_pal_wifi_scan_request {
    char ssid[H2_PAL_WIFI_SSID_MAX + 1];
    size_t ssid_len;
    uint8_t channel;
} h2_pal_wifi_scan_request_t;

typedef struct h2_pal_wifi_scan_entry {
    char ssid[H2_PAL_WIFI_SSID_MAX + 1];
    size_t ssid_len;
    uint8_t bssid[6];
    uint8_t channel;
    int rssi;
    h2_pal_wifi_security_t security;
} h2_pal_wifi_scan_entry_t;

typedef struct h2_pal_wifi_sta_status {
    h2_pal_wifi_sta_state_t state;
    char ssid[H2_PAL_WIFI_SSID_MAX + 1];
    size_t ssid_len;
    uint8_t bssid[6];
    uint8_t bssid_set;
    uint8_t channel;
    int rssi;
    h2_pal_wifi_ip_info_t ip;
    uint8_t ip_valid;
    int disconnect_reason;
} h2_pal_wifi_sta_status_t;

typedef struct h2_pal_wifi_ap_config {
    char ssid[H2_PAL_WIFI_SSID_MAX + 1];
    size_t ssid_len;
    char password[H2_PAL_WIFI_PASSWORD_MAX + 1];
    size_t password_len;
    uint8_t channel;
    uint8_t max_clients;
    h2_pal_wifi_security_t security;
    uint8_t hidden;
} h2_pal_wifi_ap_config_t;

typedef struct h2_pal_wifi_ap_status {
    h2_pal_wifi_ap_state_t state;
    char ssid[H2_PAL_WIFI_SSID_MAX + 1];
    size_t ssid_len;
    uint8_t channel;
    uint8_t max_clients;
    size_t client_count;
    h2_pal_wifi_security_t security;
    uint8_t hidden;
} h2_pal_wifi_ap_status_t;

typedef struct h2_pal_wifi_ap_client {
    uint8_t mac[6];
    int rssi;
    h2_pal_wifi_ip_info_t lease;
    uint8_t lease_valid;
    int station_id;
} h2_pal_wifi_ap_client_t;

typedef struct h2_pal_wifi_sta_event {
    h2_pal_wifi_sta_status_t status;
} h2_pal_wifi_sta_event_t;

typedef struct h2_pal_wifi_ap_event {
    h2_pal_wifi_ap_status_t status;
} h2_pal_wifi_ap_event_t;

typedef struct h2_pal_wifi_ap_client_event {
    h2_pal_wifi_ap_client_t client;
} h2_pal_wifi_ap_client_event_t;

typedef struct h2_pal_wifi_sta_api h2_pal_wifi_sta_api_t;
typedef struct h2_pal_wifi_ap_api h2_pal_wifi_ap_api_t;
typedef h2_pal_wifi_sta_api_t h2_pal_wifi_sta_t;
typedef h2_pal_wifi_ap_api_t h2_pal_wifi_ap_t;

/*
 * Return true to keep scanning; return false to stop early. Note this is the
 * opposite of h2_pal_ble_scan_result_fn, which returns true to stop: both
 * Wi-Fi providers break on a false return, and leaving it undocumented already
 * cost one caller every access point but the first.
 */
typedef bool (*h2_pal_wifi_scan_result_fn)(
    void *user,
    const h2_pal_wifi_scan_entry_t *entry);

typedef int (*h2_pal_wifi_sta_get_status_fn)(
    void *user,
    h2_pal_wifi_sta_status_t *out_status);
typedef int (*h2_pal_wifi_sta_scan_fn)(
    void *user,
    const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result,
    void *callback_user,
    uint32_t timeout_ms);
typedef int (*h2_pal_wifi_sta_connect_fn)(
    void *user,
    const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms);
typedef int (*h2_pal_wifi_sta_disconnect_fn)(void *user);
typedef int (*h2_pal_wifi_sta_get_mac_fn)(void *user, uint8_t out_mac[6]);
typedef int (*h2_pal_wifi_sta_set_power_save_fn)(
    void *user,
    h2_pal_wifi_power_save_t mode);

typedef struct h2_pal_wifi_sta_vtable {
    h2_pal_wifi_sta_get_status_fn get_status;
    h2_pal_wifi_sta_scan_fn scan;
    h2_pal_wifi_sta_connect_fn connect;
    h2_pal_wifi_sta_disconnect_fn disconnect;
    h2_pal_wifi_sta_get_mac_fn get_mac;
    /** Optional; NULL reports H2_PAL_ERR_UNSUPPORTED. */
    h2_pal_wifi_sta_set_power_save_fn set_power_save;
} h2_pal_wifi_sta_vtable_t;

struct h2_pal_wifi_sta_api {
    void *user;
    const h2_pal_wifi_sta_vtable_t *vtable;
};

typedef int (*h2_pal_wifi_ap_start_fn)(
    void *user,
    const h2_pal_wifi_ap_config_t *config,
    uint32_t timeout_ms);
typedef int (*h2_pal_wifi_ap_stop_fn)(void *user, uint32_t timeout_ms);
typedef int (*h2_pal_wifi_ap_get_status_fn)(
    void *user,
    h2_pal_wifi_ap_status_t *out_status);
typedef int (*h2_pal_wifi_ap_get_clients_fn)(
    void *user,
    h2_pal_wifi_ap_client_t *out_clients,
    size_t max_clients,
    size_t *out_count);
typedef int (*h2_pal_wifi_ap_get_mac_fn)(void *user, uint8_t out_mac[6]);

typedef struct h2_pal_wifi_ap_vtable {
    h2_pal_wifi_ap_start_fn start;
    h2_pal_wifi_ap_stop_fn stop;
    h2_pal_wifi_ap_get_status_fn get_status;
    h2_pal_wifi_ap_get_clients_fn get_clients;
    h2_pal_wifi_ap_get_mac_fn get_mac;
} h2_pal_wifi_ap_vtable_t;

struct h2_pal_wifi_ap_api {
    void *user;
    const h2_pal_wifi_ap_vtable_t *vtable;
};

static inline int h2_pal_wifi_scan_request_validate(
    const h2_pal_wifi_scan_request_t *request) {
    if (request == NULL) {
        return H2_PAL_OK;
    }
    if (request->ssid_len > H2_PAL_WIFI_SSID_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static inline int h2_pal_wifi_ap_config_validate(
    const h2_pal_wifi_ap_config_t *config) {
    if (config == NULL ||
        config->ssid_len == 0u ||
        config->ssid_len > H2_PAL_WIFI_SSID_MAX ||
        config->password_len > H2_PAL_WIFI_PASSWORD_MAX ||
        config->max_clients > H2_PAL_WIFI_AP_MAX_CLIENTS) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->security != H2_PAL_WIFI_SECURITY_OPEN && config->password_len < 8u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static inline int h2_pal_wifi_sta_get_status(
    const h2_pal_wifi_sta_api_t *sta,
    h2_pal_wifi_sta_status_t *out_status) {
    if (sta == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (sta->vtable == NULL || sta->vtable->get_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return sta->vtable->get_status(sta->user, out_status);
}

static inline int h2_pal_wifi_sta_scan(
    const h2_pal_wifi_sta_api_t *sta,
    const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result,
    void *user,
    uint32_t timeout_ms) {
    if (sta == NULL || on_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_pal_wifi_scan_request_validate(request);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (sta->vtable == NULL || sta->vtable->scan == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return sta->vtable->scan(sta->user, request, on_result, user, timeout_ms);
}

static inline int h2_pal_wifi_sta_connect(
    const h2_pal_wifi_sta_api_t *sta,
    const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms) {
    if (sta == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_pal_wifi_settings_validate_sta_config(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (sta->vtable == NULL || sta->vtable->connect == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return sta->vtable->connect(sta->user, config, timeout_ms);
}

static inline int h2_pal_wifi_sta_disconnect(const h2_pal_wifi_sta_api_t *sta) {
    if (sta == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (sta->vtable == NULL || sta->vtable->disconnect == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return sta->vtable->disconnect(sta->user);
}

static inline int h2_pal_wifi_sta_get_mac(const h2_pal_wifi_sta_api_t *sta, uint8_t out_mac[6]) {
    if (sta == NULL || out_mac == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (sta->vtable == NULL || sta->vtable->get_mac == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return sta->vtable->get_mac(sta->user, out_mac);
}

static inline int h2_pal_wifi_power_save_is_valid(h2_pal_wifi_power_save_t mode) {
    return mode == H2_PAL_WIFI_POWER_SAVE_NONE ||
           mode == H2_PAL_WIFI_POWER_SAVE_MIN_MODEM ||
           mode == H2_PAL_WIFI_POWER_SAVE_MAX_MODEM;
}

/**
 * Select the station power-save policy for the current and future links.
 */
static inline int h2_pal_wifi_sta_set_power_save(
    const h2_pal_wifi_sta_api_t *sta,
    h2_pal_wifi_power_save_t mode) {
    if (sta == NULL || !h2_pal_wifi_power_save_is_valid(mode)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (sta->vtable == NULL || sta->vtable->set_power_save == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return sta->vtable->set_power_save(sta->user, mode);
}

static inline int h2_pal_wifi_ap_start(
    const h2_pal_wifi_ap_api_t *ap,
    const h2_pal_wifi_ap_config_t *config,
    uint32_t timeout_ms) {
    if (ap == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_pal_wifi_ap_config_validate(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ap->vtable == NULL || ap->vtable->start == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ap->vtable->start(ap->user, config, timeout_ms);
}

static inline int h2_pal_wifi_ap_stop(const h2_pal_wifi_ap_api_t *ap, uint32_t timeout_ms) {
    if (ap == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ap->vtable == NULL || ap->vtable->stop == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ap->vtable->stop(ap->user, timeout_ms);
}

static inline int h2_pal_wifi_ap_get_status(
    const h2_pal_wifi_ap_api_t *ap,
    h2_pal_wifi_ap_status_t *out_status) {
    if (ap == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ap->vtable == NULL || ap->vtable->get_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ap->vtable->get_status(ap->user, out_status);
}

static inline int h2_pal_wifi_ap_get_clients(
    const h2_pal_wifi_ap_api_t *ap,
    h2_pal_wifi_ap_client_t *out_clients,
    size_t max_clients,
    size_t *out_count) {
    if (ap == NULL || out_count == NULL || (max_clients > 0u && out_clients == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ap->vtable == NULL || ap->vtable->get_clients == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ap->vtable->get_clients(ap->user, out_clients, max_clients, out_count);
}

static inline int h2_pal_wifi_ap_get_mac(const h2_pal_wifi_ap_api_t *ap, uint8_t out_mac[6]) {
    if (ap == NULL || out_mac == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ap->vtable == NULL || ap->vtable->get_mac == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ap->vtable->get_mac(ap->user, out_mac);
}

#ifdef __cplusplus
}
#endif

#endif
