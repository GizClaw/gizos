#include "h2_bk_platform_core.h"

#include <common/bk_err.h>
#include <components/event.h>
#include <components/netif.h>
#include <modules/wifi.h>
#include <os/mem.h>
#include <os/os.h>

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

static beken_semaphore_t s_h2_bk_wifi_scan_sem;
static int s_h2_bk_wifi_events_registered;
static int s_h2_bk_wifi_ap_active;
static h2_pal_wifi_ap_status_t s_h2_bk_wifi_ap_status;
static h2_pal_wifi_ap_config_t s_h2_bk_wifi_ap_config;
static int s_h2_bk_wifi_connect_pending;
static uint32_t s_h2_bk_wifi_connect_generation;
static h2_pal_wifi_sta_status_t s_h2_bk_wifi_connect_status;
static int s_h2_bk_wifi_sta_status_valid;
static h2_pal_wifi_sta_status_t s_h2_bk_wifi_sta_status;
static int s_h2_bk_wifi_last_config_valid;
static h2_pal_wifi_sta_config_t s_h2_bk_wifi_last_config;
static StaticSemaphore_t s_h2_bk_wifi_request_mutex_control;
static beken_mutex_t s_h2_bk_wifi_request_mutex;

static int h2_bk_wifi_request_lock(void) {
    return s_h2_bk_wifi_request_mutex != NULL &&
        rtos_lock_mutex(&s_h2_bk_wifi_request_mutex) == kNoErr
        ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static void h2_bk_wifi_request_unlock(void) {
    if (s_h2_bk_wifi_request_mutex != NULL) {
        (void)rtos_unlock_mutex(&s_h2_bk_wifi_request_mutex);
    }
}

static int h2_bk_wifi_sta_get_status(
    h2_pal_wifi_sta_t *sta,
    h2_pal_wifi_sta_status_t *out_status);

static size_t h2_bk_wifi_strnlen(const char *value, size_t max_len) {
    size_t len = 0u;
    while (len < max_len && value[len] != '\0') {
        len++;
    }
    return len;
}

static int h2_bk_wifi_parse_ip4(const char *value, uint32_t *out_ip) {
    if (value == NULL || out_ip == NULL) {
        return 0;
    }

    uint32_t octets[4] = {0};
    const char *cursor = value;
    for (size_t i = 0u; i < 4u; ++i) {
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }

        uint32_t octet = 0u;
        while (*cursor >= '0' && *cursor <= '9') {
            octet = (octet * 10u) + (uint32_t)(*cursor - '0');
            if (octet > 255u) {
                return 0;
            }
            cursor++;
        }
        octets[i] = octet;

        if (i < 3u) {
            if (*cursor != '.') {
                return 0;
            }
            cursor++;
        }
    }

    if (*cursor != '\0') {
        return 0;
    }

    *out_ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 1;
}

static int h2_bk_wifi_map_error(bk_err_t err) {
    switch (err) {
    case BK_OK:
        return H2_PAL_OK;
    case BK_ERR_PARAM:
    case BK_ERR_NULL_PARAM:
    case BK_ERR_WIFI_CHAN_RANGE:
    case BK_ERR_WIFI_CHAN_NUMBER:
    case BK_ERR_WIFI_RESERVED_FIELD:
        return H2_PAL_ERR_INVALID_ARG;
    case BK_ERR_WIFI_NOT_INIT:
        return H2_PAL_ERR_UNAVAILABLE;
    case BK_ERR_NOT_SUPPORT:
        return H2_PAL_ERR_UNSUPPORTED;
    case BK_ERR_NO_MEM:
        return H2_PAL_ERR_NO_MEMORY;
    case BK_ERR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    case BK_ERR_BUSY:
    case BK_ERR_IN_PROGRESS:
    case BK_ERR_STATE:
    case BK_ERR_WIFI_MONITOR_IP:
        return H2_PAL_ERR_INVALID_STATE;
    case BK_ERR_NOT_FOUND:
    case BK_ERR_WIFI_STA_NOT_CONFIG:
    case BK_ERR_WIFI_STA_NOT_STARTED:
    case BK_ERR_WIFI_AP_NOT_CONFIG:
    case BK_ERR_WIFI_AP_NOT_STARTED:
        return H2_PAL_ERR_NOT_FOUND;
    default:
        return H2_PAL_ERR_IO;
    }
}

static void h2_bk_wifi_post_system_event_payload(
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size) {
    const h2_pal_system_event_api_t *api = h2_bk_platform_system_event_api();
    if (api == NULL) {
        return;
    }

    h2_pal_system_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    if (payload != NULL && payload_size > 0u) {
        event.payload = payload;
        event.payload_size = payload_size;
    }
    (void)h2_pal_system_event_post(api, &event, 0u);
}

static void h2_bk_wifi_post_sta_system_event(
    h2_pal_system_event_type_t type,
    const h2_pal_wifi_sta_status_t *status) {
    h2_bk_wifi_post_system_event_payload(type, status, status != NULL ? sizeof(*status) : 0u);
}

static void h2_bk_wifi_store_sta_status(
    const h2_pal_wifi_sta_status_t *status) {
    if (status == NULL) {
        return;
    }
    s_h2_bk_wifi_sta_status = *status;
    __atomic_store_n(
        &s_h2_bk_wifi_sta_status_valid,
        1,
        __ATOMIC_RELEASE);
}

static int h2_bk_wifi_load_sta_status(
    h2_pal_wifi_sta_status_t *out_status) {
    if (out_status == NULL ||
        __atomic_load_n(
            &s_h2_bk_wifi_sta_status_valid,
            __ATOMIC_ACQUIRE) == 0) {
        return 0;
    }
    *out_status = s_h2_bk_wifi_sta_status;
    return 1;
}

static void h2_bk_wifi_post_ap_system_event(
    h2_pal_system_event_type_t type,
    const h2_pal_wifi_ap_status_t *status) {
    h2_pal_wifi_ap_event_t event;
    memset(&event, 0, sizeof(event));
    if (status != NULL) {
        event.status = *status;
    }
    h2_bk_wifi_post_system_event_payload(type, &event, sizeof(event));
}

static void h2_bk_wifi_post_ap_client_system_event(
    h2_pal_system_event_type_t type,
    const h2_pal_wifi_ap_client_t *client) {
    h2_pal_wifi_ap_client_event_t event;
    memset(&event, 0, sizeof(event));
    if (client != NULL) {
        event.client = *client;
    }
    h2_bk_wifi_post_system_event_payload(type, &event, sizeof(event));
}

static h2_pal_wifi_security_t h2_bk_wifi_security(wifi_security_t security) {
    switch (security) {
    case WIFI_SECURITY_NONE:
        return H2_PAL_WIFI_SECURITY_OPEN;
    case WIFI_SECURITY_WEP:
        return H2_PAL_WIFI_SECURITY_WEP;
    case WIFI_SECURITY_WPA_TKIP:
    case WIFI_SECURITY_WPA_AES:
    case WIFI_SECURITY_WPA_MIXED:
        return H2_PAL_WIFI_SECURITY_WPA;
    case WIFI_SECURITY_WPA2_TKIP:
    case WIFI_SECURITY_WPA2_AES:
    case WIFI_SECURITY_WPA2_MIXED:
        return H2_PAL_WIFI_SECURITY_WPA2;
    case WIFI_SECURITY_WPA3_SAE:
        return H2_PAL_WIFI_SECURITY_WPA3;
    case WIFI_SECURITY_WPA3_WPA2_MIXED:
        return H2_PAL_WIFI_SECURITY_WPA2_WPA3;
    case WIFI_SECURITY_EAP:
        return H2_PAL_WIFI_SECURITY_ENTERPRISE;
    default:
        return H2_PAL_WIFI_SECURITY_UNKNOWN;
    }
}

static wifi_security_t h2_bk_wifi_security_to_sdk(h2_pal_wifi_security_t security) {
    switch (security) {
    case H2_PAL_WIFI_SECURITY_OPEN:
        return WIFI_SECURITY_NONE;
    case H2_PAL_WIFI_SECURITY_WPA:
        return WIFI_SECURITY_WPA_AES;
    case H2_PAL_WIFI_SECURITY_WPA2:
        return WIFI_SECURITY_WPA2_AES;
    case H2_PAL_WIFI_SECURITY_WPA3:
        return WIFI_SECURITY_WPA3_SAE;
    case H2_PAL_WIFI_SECURITY_WPA2_WPA3:
        return WIFI_SECURITY_WPA3_WPA2_MIXED;
    default:
        return WIFI_SECURITY_WPA2_AES;
    }
}

static void h2_bk_wifi_copy_ap_client(
    h2_pal_wifi_ap_client_t *out_client,
    const wlan_ap_sta_t *sta) {
    memset(out_client, 0, sizeof(*out_client));
    memcpy(out_client->mac, sta->addr, sizeof(out_client->mac));
    out_client->rssi = sta->rssi;
    if (sta->ipaddr != 0u) {
        out_client->lease.ip4 = sta->ipaddr;
        out_client->lease_valid = 1u;
    }
}

static int h2_bk_wifi_get_ap_client_by_mac(
    const uint8_t mac[6],
    h2_pal_wifi_ap_client_t *out_client) {
    wlan_ap_stas_t stas;
    memset(&stas, 0, sizeof(stas));
    bk_err_t err = bk_wifi_ap_get_sta_list(&stas);
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }
    int rc = H2_PAL_ERR_NOT_FOUND;
    if (stas.sta != NULL) {
        for (int i = 0; i < stas.num; ++i) {
            if (memcmp(stas.sta[i].addr, mac, 6u) == 0) {
                h2_bk_wifi_copy_ap_client(out_client, &stas.sta[i]);
                rc = H2_PAL_OK;
                break;
            }
        }
    }
    if (stas.sta != NULL) {
        os_free(stas.sta);
    }
    return rc;
}

static h2_pal_wifi_sta_state_t h2_bk_wifi_state(wifi_link_state_t state) {
    switch (state) {
    case WIFI_LINKSTATE_STA_IDLE:
        return H2_PAL_WIFI_STA_STATE_IDLE;
    case WIFI_LINKSTATE_STA_CONNECTING:
        return H2_PAL_WIFI_STA_STATE_CONNECTING;
    case WIFI_LINKSTATE_STA_CONNECTED:
        return H2_PAL_WIFI_STA_STATE_CONNECTED;
    case WIFI_LINKSTATE_STA_GOT_IP:
        return H2_PAL_WIFI_STA_STATE_GOT_IP;
    case WIFI_LINKSTATE_STA_DISCONNECTED:
        return H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    case WIFI_LINKSTATE_STA_CONNECT_FAILED:
        return H2_PAL_WIFI_STA_STATE_FAILED;
    case WIFI_LINKSTATE_STA_SCAN_DONE:
        return H2_PAL_WIFI_STA_STATE_SCANNING;
    default:
        return H2_PAL_WIFI_STA_STATE_UNKNOWN;
    }
}

static void h2_bk_wifi_fill_sta_ip(h2_pal_wifi_sta_status_t *status) {
    if (status == NULL) {
        return;
    }

    netif_ip4_config_t ip4_config;
    memset(&ip4_config, 0, sizeof(ip4_config));
    if (bk_netif_get_ip4_config(NETIF_IF_STA, &ip4_config) != BK_OK) {
        return;
    }

    uint32_t ip4 = 0u;
    if (h2_bk_wifi_parse_ip4(ip4_config.ip, &ip4) == 0 || ip4 == 0u) {
        return;
    }

    status->ip.ip4 = ip4;
    (void)h2_bk_wifi_parse_ip4(ip4_config.mask, &status->ip.netmask4);
    (void)h2_bk_wifi_parse_ip4(ip4_config.gateway, &status->ip.gateway4);
    status->ip_valid = 1u;
}

static bk_err_t h2_bk_wifi_system_event_handler(
    void *arg,
    event_module_t event_module,
    int event_id,
    void *event_data) {
    (void)arg;
    if (event_module == EVENT_MOD_NETIF) {
        const netif_event_got_ip4_t *netif_event = (const netif_event_got_ip4_t *)event_data;
        if (netif_event == NULL || netif_event->netif_if != NETIF_IF_STA) {
            return BK_OK;
        }

        if (event_id == EVENT_NETIF_GOT_IP4) {
            h2_pal_wifi_sta_status_t status;
            memset(&status, 0, sizeof(status));
            if (h2_bk_wifi_sta_get_status(NULL, &status) != H2_PAL_OK) {
                status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
                h2_bk_wifi_fill_sta_ip(&status);
            }
            status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
            if (__atomic_load_n(
                    &s_h2_bk_wifi_last_config_valid,
                    __ATOMIC_ACQUIRE) != 0) {
                status.ssid_len = s_h2_bk_wifi_last_config.ssid_len;
                memcpy(
                    status.ssid,
                    s_h2_bk_wifi_last_config.ssid,
                    status.ssid_len);
                status.ssid[status.ssid_len] = '\0';
            }
            h2_bk_wifi_store_sta_status(&status);
            h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP, &status);
            (void)h2_bk_platform_netif_reconcile_default_async();
            return BK_OK;
        }

        if (event_id == EVENT_NETIF_DHCP_TIMEOUT) {
            h2_pal_wifi_sta_status_t status;
            memset(&status, 0, sizeof(status));
            status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
            h2_bk_wifi_store_sta_status(&status);
            h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP, &status);
            (void)h2_bk_platform_netif_reconcile_default_async();
            return BK_OK;
        }

        return BK_OK;
    }

    if (event_module != EVENT_MOD_WIFI) {
        return BK_OK;
    }

    if (event_id == EVENT_WIFI_STA_CONNECTED) {
        h2_pal_wifi_sta_status_t status;
        memset(&status, 0, sizeof(status));
        status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
        const wifi_event_sta_connected_t *connected = (const wifi_event_sta_connected_t *)event_data;
        if (connected != NULL) {
            status.ssid_len = h2_bk_wifi_strnlen(connected->ssid, H2_PAL_WIFI_SSID_MAX);
            memcpy(status.ssid, connected->ssid, status.ssid_len);
            status.ssid[status.ssid_len] = '\0';
        }
        h2_bk_wifi_store_sta_status(&status);
        h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED, &status);
        return BK_OK;
    }

    if (event_id == EVENT_WIFI_STA_DISCONNECTED) {
        h2_pal_wifi_sta_status_t status;
        memset(&status, 0, sizeof(status));
        status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
        h2_bk_wifi_store_sta_status(&status);
        h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED, &status);
        (void)h2_bk_platform_netif_reconcile_default_async();
        return BK_OK;
    }

    if (event_id == EVENT_WIFI_AP_CONNECTED) {
        const wifi_event_ap_connected_t *connected = (const wifi_event_ap_connected_t *)event_data;
        h2_pal_wifi_ap_client_t client;
        memset(&client, 0, sizeof(client));
        if (connected != NULL) {
            memcpy(client.mac, connected->mac, sizeof(client.mac));
            (void)h2_bk_wifi_get_ap_client_by_mac(connected->mac, &client);
        }
        h2_bk_wifi_post_ap_client_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_JOINED, &client);
        if (client.lease_valid != 0u) {
            h2_bk_wifi_post_ap_client_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_GRANTED, &client);
        }
        return BK_OK;
    }

    if (event_id == EVENT_WIFI_AP_DISCONNECTED) {
        const wifi_event_ap_disconnected_t *disconnected = (const wifi_event_ap_disconnected_t *)event_data;
        h2_pal_wifi_ap_client_t client;
        memset(&client, 0, sizeof(client));
        if (disconnected != NULL) {
            memcpy(client.mac, disconnected->mac, sizeof(client.mac));
        }
        h2_bk_wifi_post_ap_client_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_LEFT, &client);
        return BK_OK;
    }

    return BK_OK;
}

static int h2_bk_wifi_ensure_events_registered(void) {
    if (s_h2_bk_wifi_events_registered != 0) {
        return H2_PAL_OK;
    }

    bk_err_t err = bk_event_register_cb(
        EVENT_MOD_WIFI,
        EVENT_WIFI_STA_CONNECTED,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_WIFI,
        EVENT_WIFI_STA_DISCONNECTED,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_WIFI,
        EVENT_WIFI_AP_CONNECTED,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_WIFI,
        EVENT_WIFI_AP_DISCONNECTED,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_NETIF,
        EVENT_NETIF_GOT_IP4,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_NETIF,
        EVENT_NETIF_DHCP_TIMEOUT,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    s_h2_bk_wifi_events_registered = 1;
    return H2_PAL_OK;
}

static bk_err_t h2_bk_wifi_scan_done_handler(
    void *arg,
    event_module_t event_module,
    int event_id,
    void *event_data) {
    (void)arg;
    (void)event_module;
    (void)event_id;
    (void)event_data;

    if (s_h2_bk_wifi_scan_sem != NULL) {
        (void)rtos_set_semaphore(&s_h2_bk_wifi_scan_sem);
    }
    return BK_OK;
}

static void h2_bk_wifi_copy_scan_entry(
    h2_pal_wifi_scan_entry_t *out_entry,
    const wifi_scan_ap_info_t *ap) {
    memset(out_entry, 0, sizeof(*out_entry));
    out_entry->ssid_len = h2_bk_wifi_strnlen(ap->ssid, H2_PAL_WIFI_SSID_MAX);
    memcpy(out_entry->ssid, ap->ssid, out_entry->ssid_len);
    out_entry->ssid[out_entry->ssid_len] = '\0';
    memcpy(out_entry->bssid, ap->bssid, sizeof(out_entry->bssid));
    out_entry->channel = ap->channel;
    out_entry->rssi = ap->rssi;
    out_entry->security = h2_bk_wifi_security(ap->security);
}

static int h2_bk_wifi_sta_get_status(
    h2_pal_wifi_sta_t *sta,
    h2_pal_wifi_sta_status_t *out_status) {
    (void)sta;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (__atomic_load_n(
            &s_h2_bk_wifi_connect_pending,
            __ATOMIC_ACQUIRE) != 0) {
        *out_status = s_h2_bk_wifi_connect_status;
        return H2_PAL_OK;
    }
    if (h2_bk_wifi_load_sta_status(out_status) != 0 &&
        out_status->state == H2_PAL_WIFI_STA_STATE_GOT_IP &&
        out_status->ip_valid != 0u) {
        return H2_PAL_OK;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->state = H2_PAL_WIFI_STA_STATE_IDLE;

    wifi_link_status_t link_status;
    memset(&link_status, 0, sizeof(link_status));
    bk_err_t err = bk_wifi_sta_get_link_status(&link_status);
    if (err == BK_ERR_WIFI_STA_NOT_STARTED || err == BK_ERR_WIFI_STA_NOT_CONFIG) {
        return H2_PAL_OK;
    }
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }

    out_status->state = h2_bk_wifi_state(link_status.state);
    out_status->ssid_len = h2_bk_wifi_strnlen(link_status.ssid, H2_PAL_WIFI_SSID_MAX);
    memcpy(out_status->ssid, link_status.ssid, out_status->ssid_len);
    out_status->ssid[out_status->ssid_len] = '\0';
    memcpy(out_status->bssid, link_status.bssid, sizeof(out_status->bssid));
    out_status->bssid_set = out_status->ssid_len > 0u ? 1u : 0u;
    out_status->channel = link_status.channel;
    out_status->rssi = link_status.rssi;
    h2_bk_wifi_fill_sta_ip(out_status);
    if (out_status->ip_valid != 0u) {
        out_status->state = H2_PAL_WIFI_STA_STATE_GOT_IP;
    }
    h2_bk_wifi_store_sta_status(out_status);
    return H2_PAL_OK;
}

static int h2_bk_wifi_sta_scan(
    h2_pal_wifi_sta_t *sta,
    const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result,
    void *user,
    uint32_t timeout_ms) {
    (void)sta;
    if (on_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    int register_rc = h2_bk_wifi_ensure_events_registered();
    if (register_rc != H2_PAL_OK) {
        return register_rc;
    }

    wifi_scan_config_t scan_config;
    memset(&scan_config, 0, sizeof(scan_config));
    if (request != NULL && request->ssid_len > 0u) {
        memcpy(scan_config.ssid, request->ssid, request->ssid_len);
        scan_config.ssid[request->ssid_len] = '\0';
    }
    if (request != NULL && request->channel != 0u) {
        scan_config.chan_cnt = 1u;
        scan_config.chan_nb[0] = request->channel;
    }

    bk_err_t err = rtos_init_semaphore(&s_h2_bk_wifi_scan_sem, 1);
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(EVENT_MOD_WIFI, EVENT_WIFI_SCAN_DONE, h2_bk_wifi_scan_done_handler, NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        rtos_deinit_semaphore(&s_h2_bk_wifi_scan_sem);
        return h2_bk_wifi_map_error(err);
    }

    err = bk_wifi_scan_start(&scan_config);
    if (err == BK_OK) {
        uint32_t wait_ms = timeout_ms != 0u ? timeout_ms : 10000u;
        err = rtos_get_semaphore(&s_h2_bk_wifi_scan_sem, wait_ms);
        if (err == BK_OK) {
            wifi_scan_result_t scan_result;
            memset(&scan_result, 0, sizeof(scan_result));
            err = bk_wifi_scan_get_result(&scan_result);
            if (err == BK_OK) {
                size_t copy_count = (size_t)scan_result.ap_num;
                for (size_t i = 0u; i < copy_count; ++i) {
                    h2_pal_wifi_scan_entry_t entry;
                    h2_bk_wifi_copy_scan_entry(&entry, &scan_result.aps[i]);
                    if (!on_result(user, &entry)) {
                        break;
                    }
                }
                bk_wifi_scan_free_result(&scan_result);
            }
        }
    }

    (void)bk_event_unregister_cb(EVENT_MOD_WIFI, EVENT_WIFI_SCAN_DONE, h2_bk_wifi_scan_done_handler);
    rtos_deinit_semaphore(&s_h2_bk_wifi_scan_sem);
    s_h2_bk_wifi_scan_sem = NULL;
    return h2_bk_wifi_map_error(err);
}

static void h2_bk_wifi_copy_sta_config(
    wifi_sta_config_t *out_config,
    const h2_pal_wifi_sta_config_t *config) {
    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config->ssid, config->ssid, config->ssid_len);
    out_config->ssid[config->ssid_len] = '\0';
    memcpy(out_config->password, config->password, config->password_len);
    out_config->password[config->password_len] = '\0';
    if (config->bssid_set != 0u) {
        memcpy(out_config->bssid, config->bssid, sizeof(out_config->bssid));
    }
    out_config->channel = config->channel;
    out_config->security = WIFI_SECURITY_AUTO;
    out_config->auto_reconnect_count = 0;
    out_config->auto_reconnect_timeout = 0;
}

static int h2_bk_wifi_sta_is_connected(
    const wifi_link_status_t *link_status,
    const h2_pal_wifi_sta_config_t *config) {
    if (link_status->state != WIFI_LINKSTATE_STA_CONNECTED &&
        link_status->state != WIFI_LINKSTATE_STA_GOT_IP) {
        return 0;
    }
    size_t ssid_len =
        h2_bk_wifi_strnlen(link_status->ssid, H2_PAL_WIFI_SSID_MAX);
    return ssid_len == config->ssid_len &&
        memcmp(link_status->ssid, config->ssid, ssid_len) == 0;
}

typedef struct h2_bk_wifi_connect_request {
    h2_pal_wifi_sta_config_t requested_config;
    wifi_sta_config_t sdk_config;
    uint32_t generation;
} h2_bk_wifi_connect_request_t;

static void h2_bk_wifi_connect_worker(void *arg) {
    h2_bk_wifi_connect_request_t *request = arg;
    int has_request = request != NULL;
    uint32_t generation = has_request ? request->generation : 0u;
    if (has_request) {
        /* Starting BK Wi-Fi briefly stalls the AP/CP command transport. Give
         * the caller time to deliver the accepted response first. */
        rtos_delay_milliseconds(500u);
        for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
            if (__atomic_load_n(
                    &s_h2_bk_wifi_connect_generation,
                    __ATOMIC_ACQUIRE) != generation) {
                break;
            }
            wifi_link_status_t current_status;
            memset(&current_status, 0, sizeof(current_status));
            bk_err_t err = bk_wifi_sta_get_link_status(&current_status);
            if (err == BK_OK && h2_bk_wifi_sta_is_connected(
                    &current_status, &request->requested_config)) {
                break;
            }
            int current_generation = 0;
            int started = 0;
            bk_err_t start_err = BK_FAIL;
            if (h2_bk_wifi_request_lock() == H2_PAL_OK) {
                current_generation = __atomic_load_n(
                    &s_h2_bk_wifi_connect_generation,
                    __ATOMIC_ACQUIRE) == generation;
                if (current_generation) {
                    start_err = bk_wifi_sta_set_config(&request->sdk_config);
                    if (start_err == BK_OK) {
                        start_err = bk_wifi_sta_start();
                        started = start_err == BK_OK;
                    }
                    if (start_err == BK_OK) {
                        start_err = bk_wifi_sta_connect();
                    }
                }
                h2_bk_wifi_request_unlock();
            }
            if (!current_generation) {
                goto done;
            }
            if (start_err == BK_OK) {
                for (uint32_t elapsed = 0u; elapsed < 15000u; elapsed += 100u) {
                    if (__atomic_load_n(
                            &s_h2_bk_wifi_connect_generation,
                            __ATOMIC_ACQUIRE) != generation) {
                        goto done;
                    }
                    memset(&current_status, 0, sizeof(current_status));
                    if (bk_wifi_sta_get_link_status(&current_status) == BK_OK &&
                        h2_bk_wifi_sta_is_connected(
                            &current_status,
                            &request->requested_config)) {
                        goto done;
                    }
                    rtos_delay_milliseconds(100u);
                }
            }
            if (started) {
                (void)bk_wifi_sta_stop();
            }
            rtos_delay_milliseconds(500u);
        }
done:
        os_free(request);
    }
    if (has_request && h2_bk_wifi_request_lock() == H2_PAL_OK) {
        if (__atomic_load_n(
                &s_h2_bk_wifi_connect_generation,
                __ATOMIC_ACQUIRE) == generation) {
            __atomic_store_n(
                &s_h2_bk_wifi_connect_pending,
                0,
                __ATOMIC_RELEASE);
        }
        h2_bk_wifi_request_unlock();
    }
    rtos_delete_thread(NULL);
}

static int h2_bk_wifi_sta_connect(
    h2_pal_wifi_sta_t *sta,
    const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms) {
    (void)sta;
    int rc = h2_pal_wifi_settings_validate_sta_config(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    rc = h2_bk_wifi_ensure_events_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    rc = h2_bk_wifi_request_lock();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    if (__atomic_load_n(
            &s_h2_bk_wifi_connect_pending,
            __ATOMIC_ACQUIRE) != 0) {
        rc = timeout_ms == 0u &&
            s_h2_bk_wifi_connect_status.ssid_len == config->ssid_len &&
            memcmp(
                s_h2_bk_wifi_connect_status.ssid,
                config->ssid,
                config->ssid_len) == 0
            ? H2_PAL_OK
            : H2_PAL_ERR_BUSY;
        h2_bk_wifi_request_unlock();
        return rc;
    }

    h2_pal_wifi_sta_status_t cached_status;
    memset(&cached_status, 0, sizeof(cached_status));
    if (__atomic_load_n(
            &s_h2_bk_wifi_last_config_valid,
            __ATOMIC_ACQUIRE) != 0 &&
        s_h2_bk_wifi_last_config.ssid_len == config->ssid_len &&
        memcmp(
            s_h2_bk_wifi_last_config.ssid,
            config->ssid,
            config->ssid_len) == 0) {
        h2_bk_wifi_fill_sta_ip(&cached_status);
        if (cached_status.ip_valid != 0u) {
            cached_status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
            cached_status.ssid_len = config->ssid_len;
            memcpy(cached_status.ssid, config->ssid, config->ssid_len);
            cached_status.ssid[config->ssid_len] = '\0';
            h2_bk_wifi_store_sta_status(&cached_status);
            h2_bk_wifi_request_unlock();
            return H2_PAL_OK;
        }
    }
    if (h2_bk_wifi_load_sta_status(&cached_status) != 0 &&
        cached_status.state == H2_PAL_WIFI_STA_STATE_GOT_IP &&
        cached_status.ip_valid != 0u &&
        cached_status.ssid_len == config->ssid_len &&
        memcmp(cached_status.ssid, config->ssid, config->ssid_len) == 0) {
        h2_bk_wifi_request_unlock();
        return H2_PAL_OK;
    }

    wifi_sta_config_t bk_config;
    h2_bk_wifi_copy_sta_config(&bk_config, config);
    s_h2_bk_wifi_last_config = *config;
    __atomic_store_n(
        &s_h2_bk_wifi_last_config_valid,
        1,
        __ATOMIC_RELEASE);
    bk_err_t err = BK_OK;
    if (timeout_ms == 0u) {
        memset(
            &s_h2_bk_wifi_connect_status,
            0,
            sizeof(s_h2_bk_wifi_connect_status));
        s_h2_bk_wifi_connect_status.state =
            H2_PAL_WIFI_STA_STATE_CONNECTING;
        s_h2_bk_wifi_connect_status.ssid_len = config->ssid_len;
        memcpy(
            s_h2_bk_wifi_connect_status.ssid,
            config->ssid,
            config->ssid_len);
        s_h2_bk_wifi_connect_status.ssid[config->ssid_len] = '\0';
        __atomic_store_n(
            &s_h2_bk_wifi_connect_pending,
            1,
            __ATOMIC_RELEASE);
        h2_bk_wifi_connect_request_t *request =
            os_malloc(sizeof(*request));
        if (request == NULL) {
            __atomic_store_n(
                &s_h2_bk_wifi_connect_pending,
                0,
                __ATOMIC_RELEASE);
            h2_bk_wifi_request_unlock();
            return H2_PAL_ERR_NO_MEMORY;
        }
        request->requested_config = *config;
        request->sdk_config = bk_config;
        request->generation = __atomic_add_fetch(
            &s_h2_bk_wifi_connect_generation,
            1u,
            __ATOMIC_ACQ_REL);
        if (rtos_create_thread(
                NULL,
                BEKEN_APPLICATION_PRIORITY,
                "h2_wifi_conn",
                h2_bk_wifi_connect_worker,
                4096u,
                request) != kNoErr) {
            os_free(request);
            __atomic_store_n(
                &s_h2_bk_wifi_connect_pending,
                0,
                __ATOMIC_RELEASE);
            h2_bk_wifi_request_unlock();
            return H2_PAL_ERR_NO_MEMORY;
        }
        h2_bk_wifi_request_unlock();
    } else {
        h2_bk_wifi_request_unlock();
        wifi_link_status_t current_status;
        memset(&current_status, 0, sizeof(current_status));
        err = bk_wifi_sta_get_link_status(&current_status);
        if (err == BK_OK &&
            h2_bk_wifi_sta_is_connected(&current_status, config)) {
            return H2_PAL_OK;
        }
        if (err != BK_OK &&
            err != BK_ERR_WIFI_STA_NOT_STARTED &&
            err != BK_ERR_WIFI_STA_NOT_CONFIG) {
            return h2_bk_wifi_map_error(err);
        }
        err = bk_wifi_sta_set_config(&bk_config);
        if (err != BK_OK) {
            return h2_bk_wifi_map_error(err);
        }

        err = bk_wifi_sta_start();
        if (err != BK_OK) {
            return h2_bk_wifi_map_error(err);
        }
        err = bk_wifi_sta_connect();
        if (err != BK_OK) {
            return h2_bk_wifi_map_error(err);
        }
    }
    h2_pal_wifi_sta_status_t connecting_status;
    memset(&connecting_status, 0, sizeof(connecting_status));
    connecting_status.state = H2_PAL_WIFI_STA_STATE_CONNECTING;
    connecting_status.ssid_len = config->ssid_len;
    memcpy(connecting_status.ssid, config->ssid, config->ssid_len);
    connecting_status.ssid[connecting_status.ssid_len] = '\0';
    h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING, &connecting_status);
    if (timeout_ms == 0u) {
        return H2_PAL_OK;
    }

    uint32_t elapsed = 0u;
    while (elapsed < timeout_ms) {
        wifi_link_status_t link_status;
        memset(&link_status, 0, sizeof(link_status));
        err = bk_wifi_sta_get_link_status(&link_status);
        if (err == BK_OK && h2_bk_wifi_sta_is_connected(
                &link_status, config)) {
            return H2_PAL_OK;
        }
        if (err != BK_OK &&
            err != BK_ERR_WIFI_STA_NOT_STARTED &&
            err != BK_ERR_WIFI_STA_NOT_CONFIG) {
            return h2_bk_wifi_map_error(err);
        }
        rtos_delay_milliseconds(100u);
        elapsed += 100u;
    }
    return H2_PAL_ERR_TIMEOUT;
}

static int h2_bk_wifi_sta_disconnect(h2_pal_wifi_sta_t *sta) {
    (void)sta;
    int lock_rc = h2_bk_wifi_request_lock();
    if (lock_rc != H2_PAL_OK) {
        return lock_rc;
    }
    (void)__atomic_add_fetch(
        &s_h2_bk_wifi_connect_generation,
        1u,
        __ATOMIC_ACQ_REL);
    __atomic_store_n(
        &s_h2_bk_wifi_connect_pending,
        0,
        __ATOMIC_RELEASE);
    h2_bk_wifi_request_unlock();
    /* BK's disconnect leaves the STA service and netif allocated. A later
     * start is then ignored, which can restore an IP-looking link without a
     * usable default route. Stop fully so the next connect recreates both. */
    bk_err_t err = bk_wifi_sta_stop();
    if (err == BK_ERR_WIFI_STA_NOT_STARTED || err == BK_ERR_WIFI_STA_NOT_CONFIG) {
        return H2_PAL_OK;
    }
    int rc = h2_bk_wifi_map_error(err);
    if (rc == H2_PAL_OK) {
        h2_pal_wifi_sta_status_t status;
        memset(&status, 0, sizeof(status));
        status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
        h2_bk_wifi_store_sta_status(&status);
        h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED, &status);
    }
    return rc;
}

static int h2_bk_wifi_sta_get_mac(h2_pal_wifi_sta_t *sta, uint8_t out_mac[6]) {
    (void)sta;
    if (out_mac == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_bk_wifi_map_error(bk_wifi_sta_get_mac(out_mac));
}

static void h2_bk_wifi_copy_ap_config(
    wifi_ap_config_t *out_config,
    const h2_pal_wifi_ap_config_t *config) {
    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config->ssid, config->ssid, config->ssid_len);
    memcpy(out_config->password, config->password, config->password_len);
    out_config->channel = config->channel != 0u ? config->channel : 1u;
    out_config->security = h2_bk_wifi_security_to_sdk(config->security);
    out_config->hidden = config->hidden != 0u ? 1u : 0u;
    out_config->max_con = config->max_clients != 0u ? config->max_clients : H2_PAL_WIFI_AP_MAX_CLIENTS;
    if (config->security == H2_PAL_WIFI_SECURITY_OPEN) {
        out_config->security = WIFI_SECURITY_NONE;
        out_config->password[0] = '\0';
    }
}

static void h2_bk_wifi_set_ap_status_from_config(const h2_pal_wifi_ap_config_t *config) {
    memset(&s_h2_bk_wifi_ap_status, 0, sizeof(s_h2_bk_wifi_ap_status));
    s_h2_bk_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STARTED;
    s_h2_bk_wifi_ap_status.ssid_len = config->ssid_len;
    memcpy(s_h2_bk_wifi_ap_status.ssid, config->ssid, config->ssid_len);
    s_h2_bk_wifi_ap_status.ssid[s_h2_bk_wifi_ap_status.ssid_len] = '\0';
    s_h2_bk_wifi_ap_status.channel = config->channel != 0u ? config->channel : 1u;
    s_h2_bk_wifi_ap_status.max_clients = config->max_clients != 0u ? config->max_clients : H2_PAL_WIFI_AP_MAX_CLIENTS;
    s_h2_bk_wifi_ap_status.security = config->security;
    s_h2_bk_wifi_ap_status.hidden = config->hidden;
}

static int h2_bk_wifi_ap_config_same(const h2_pal_wifi_ap_config_t *config) {
    return s_h2_bk_wifi_ap_active != 0 &&
        s_h2_bk_wifi_ap_config.ssid_len == config->ssid_len &&
        s_h2_bk_wifi_ap_config.password_len == config->password_len &&
        s_h2_bk_wifi_ap_config.channel == config->channel &&
        s_h2_bk_wifi_ap_config.max_clients == config->max_clients &&
        s_h2_bk_wifi_ap_config.security == config->security &&
        s_h2_bk_wifi_ap_config.hidden == config->hidden &&
        memcmp(s_h2_bk_wifi_ap_config.ssid, config->ssid, config->ssid_len) == 0 &&
        memcmp(s_h2_bk_wifi_ap_config.password, config->password, config->password_len) == 0;
}

static int h2_bk_wifi_ap_start(
    h2_pal_wifi_ap_t *ap,
    const h2_pal_wifi_ap_config_t *config,
    uint32_t timeout_ms) {
    (void)ap;
    (void)timeout_ms;
    int rc = h2_pal_wifi_ap_config_validate(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (h2_bk_wifi_ap_config_same(config)) {
        return H2_PAL_OK;
    }

    rc = h2_bk_wifi_ensure_events_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    if (s_h2_bk_wifi_ap_active != 0) {
        bk_err_t stop_err = bk_wifi_ap_stop();
        if (stop_err != BK_OK && stop_err != BK_ERR_WIFI_AP_NOT_STARTED && stop_err != BK_ERR_WIFI_AP_NOT_CONFIG) {
            return h2_bk_wifi_map_error(stop_err);
        }
        s_h2_bk_wifi_ap_active = 0;
        s_h2_bk_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STOPPED;
        s_h2_bk_wifi_ap_status.client_count = 0u;
        h2_bk_wifi_post_ap_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED, &s_h2_bk_wifi_ap_status);
    }

    wifi_ap_config_t bk_config;
    h2_bk_wifi_copy_ap_config(&bk_config, config);
    bk_err_t err = bk_wifi_ap_set_config(&bk_config);
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }
    err = bk_wifi_ap_start();
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }

    s_h2_bk_wifi_ap_active = 1;
    s_h2_bk_wifi_ap_config = *config;
    h2_bk_wifi_set_ap_status_from_config(config);
    h2_bk_wifi_post_ap_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED, &s_h2_bk_wifi_ap_status);
    return H2_PAL_OK;
}

static int h2_bk_wifi_ap_stop(h2_pal_wifi_ap_t *ap, uint32_t timeout_ms) {
    (void)ap;
    (void)timeout_ms;
    int was_active = s_h2_bk_wifi_ap_active;
    bk_err_t err = bk_wifi_ap_stop();
    if (err == BK_ERR_WIFI_AP_NOT_STARTED || err == BK_ERR_WIFI_AP_NOT_CONFIG) {
        err = BK_OK;
    }
    int rc = h2_bk_wifi_map_error(err);
    if (rc == H2_PAL_OK) {
        s_h2_bk_wifi_ap_active = 0;
        s_h2_bk_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STOPPED;
        s_h2_bk_wifi_ap_status.client_count = 0u;
        if (was_active != 0) {
            h2_bk_wifi_post_ap_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED, &s_h2_bk_wifi_ap_status);
        }
    }
    return rc;
}

static int h2_bk_wifi_ap_get_status(
    h2_pal_wifi_ap_t *ap,
    h2_pal_wifi_ap_status_t *out_status) {
    (void)ap;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (s_h2_bk_wifi_ap_active == 0) {
        out_status->state = H2_PAL_WIFI_AP_STATE_STOPPED;
        return H2_PAL_OK;
    }
    *out_status = s_h2_bk_wifi_ap_status;
    wlan_ap_stas_t stas;
    memset(&stas, 0, sizeof(stas));
    bk_err_t err = bk_wifi_ap_get_sta_list(&stas);
    if (err == BK_OK) {
        out_status->client_count = stas.num > 0 ? (size_t)stas.num : 0u;
        if (stas.sta != NULL) {
            os_free(stas.sta);
        }
    }
    return H2_PAL_OK;
}

static int h2_bk_wifi_ap_get_clients(
    h2_pal_wifi_ap_t *ap,
    h2_pal_wifi_ap_client_t *out_clients,
    size_t max_clients,
    size_t *out_count) {
    (void)ap;
    if (out_count == NULL || (max_clients > 0u && out_clients == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = 0u;
    if (s_h2_bk_wifi_ap_active == 0) {
        return H2_PAL_OK;
    }

    wlan_ap_stas_t stas;
    memset(&stas, 0, sizeof(stas));
    bk_err_t err = bk_wifi_ap_get_sta_list(&stas);
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }
    size_t copy_count = stas.num > 0 ? (size_t)stas.num : 0u;
    if (copy_count > max_clients) {
        copy_count = max_clients;
    }
    if (stas.sta != NULL) {
        for (size_t i = 0u; i < copy_count; ++i) {
            h2_bk_wifi_copy_ap_client(&out_clients[i], &stas.sta[i]);
        }
    } else {
        copy_count = 0u;
    }
    if (stas.sta != NULL) {
        os_free(stas.sta);
    }
    *out_count = copy_count;
    return H2_PAL_OK;
}

static int h2_bk_wifi_ap_get_mac(h2_pal_wifi_ap_t *ap, uint8_t out_mac[6]) {
    (void)ap;
    if (out_mac == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_bk_wifi_map_error(bk_wifi_ap_get_mac(out_mac));
}

static const h2_pal_wifi_sta_vtable_t s_h2_bk_wifi_sta_vtable = {
    .get_status = (h2_pal_wifi_sta_get_status_fn)h2_bk_wifi_sta_get_status,
    .scan = (h2_pal_wifi_sta_scan_fn)h2_bk_wifi_sta_scan,
    .connect = (h2_pal_wifi_sta_connect_fn)h2_bk_wifi_sta_connect,
    .disconnect = (h2_pal_wifi_sta_disconnect_fn)h2_bk_wifi_sta_disconnect,
    .get_mac = (h2_pal_wifi_sta_get_mac_fn)h2_bk_wifi_sta_get_mac,
};

static h2_pal_wifi_sta_t s_h2_bk_wifi_sta = {
    .user = &s_h2_bk_wifi_sta,
    .vtable = &s_h2_bk_wifi_sta_vtable,
};

static const h2_pal_wifi_ap_vtable_t s_h2_bk_wifi_ap_vtable = {
    .start = (h2_pal_wifi_ap_start_fn)h2_bk_wifi_ap_start,
    .stop = (h2_pal_wifi_ap_stop_fn)h2_bk_wifi_ap_stop,
    .get_status = (h2_pal_wifi_ap_get_status_fn)h2_bk_wifi_ap_get_status,
    .get_clients = (h2_pal_wifi_ap_get_clients_fn)h2_bk_wifi_ap_get_clients,
    .get_mac = (h2_pal_wifi_ap_get_mac_fn)h2_bk_wifi_ap_get_mac,
};

static h2_pal_wifi_ap_t s_h2_bk_wifi_ap = {
    .user = &s_h2_bk_wifi_ap,
    .vtable = &s_h2_bk_wifi_ap_vtable,
};

h2_pal_wifi_sta_t *h2_bk_platform_wifi_sta(void) {
    if (s_h2_bk_wifi_request_mutex == NULL) {
        s_h2_bk_wifi_request_mutex = (beken_mutex_t)xSemaphoreCreateMutexStatic(
            &s_h2_bk_wifi_request_mutex_control);
    }
    return &s_h2_bk_wifi_sta;
}

h2_pal_wifi_ap_t *h2_bk_platform_wifi_ap(void) {
    return &s_h2_bk_wifi_ap;
}
