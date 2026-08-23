#include "h2_runtime_internal.h"

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_gpio_irq.h"
#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/hal/h2_pal_wifi.h"

#include <string.h>

_Static_assert(sizeof(h2_runtime_system_event_schema_t) <=
                   H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "system event schemas must fit the Runtime payload limit");

size_t h2_runtime_system_event_payload_capacity_min(void) {
    return sizeof(h2_runtime_system_event_schema_t);
}

static const h2_pal_system_event_type_t s_system_event_types[] = {
    H2_PAL_SYSTEM_EVENT_TYPE_GPIO_IRQ,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_JOINED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_LEFT,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_GRANTED,
    H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_RELEASED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
    H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_INDICATION,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_READY,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_ERROR,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIM_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_OPENED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_CLOSED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED,
    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED,
    H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
};

static int no_payload(const h2_pal_system_event_t *event) {
    return event->payload == NULL && event->payload_size == 0u;
}

static int payload_is(const h2_pal_system_event_t *event, size_t size) {
    return event->payload != NULL && event->payload_size == size;
}

static h2_runtime_event_kind_t system_kind_from_pal(h2_pal_system_event_type_t type) {
    switch (type) {
    case H2_PAL_SYSTEM_EVENT_TYPE_GPIO_IRQ:
        return H2_RUNTIME_SYSTEM_EVENT_GPIO_IRQ_TRIGGERED;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STOPPED;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_JOINED:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_LEFT:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_GRANTED:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_RELEASED:
        return H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STARTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STOPPED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STARTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STOPPED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTION_UPDATED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_DISCONNECTED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_MTU_CHANGED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_SUBSCRIPTION_CHANGED;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_NOTIFICATION;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_INDICATION:
        return H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_INDICATION;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_READY:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_READY;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_ERROR:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_ERROR;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIM_CHANGED:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_SIM_CHANGED;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_REGISTRATION_CHANGED;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_PACKET_CHANGED;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_SIGNAL_CHANGED;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_OPENED:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_OPENED;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_CLOSED:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_CLOSED;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_INCOMING;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_STATE_CHANGED;
    case H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED:
        return H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_ENDED;
    case H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED:
        return H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED;
    default:
        return H2_RUNTIME_EVENT_NONE;
    }
}

static h2_runtime_component_t system_component_from_kind(h2_runtime_event_kind_t kind) {
    switch (kind) {
    case H2_RUNTIME_SYSTEM_EVENT_GPIO_IRQ_TRIGGERED:
        return H2_RUNTIME_COMPONENT_SYSTEM_GPIO_IRQ;
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STOPPED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED:
        return H2_RUNTIME_COMPONENT_SYSTEM_WIFI;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STOPPED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STOPPED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTION_UPDATED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_DISCONNECTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_MTU_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SUBSCRIPTION_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_NOTIFICATION:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_INDICATION:
        return H2_RUNTIME_COMPONENT_SYSTEM_BLE;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_READY:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_ERROR:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_SIM_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_REGISTRATION_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_PACKET_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_SIGNAL_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_OPENED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_CLOSED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_INCOMING:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_STATE_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_ENDED:
        return H2_RUNTIME_COMPONENT_SYSTEM_MODEM;
    case H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED:
        return H2_RUNTIME_COMPONENT_SYSTEM_NETIF;
    default:
        return H2_RUNTIME_COMPONENT_NONE;
    }
}

static void copy_wifi_ip(
    h2_runtime_system_wifi_ip_info_t *out,
    const h2_pal_wifi_ip_info_t *in) {
    out->ip4 = in->ip4;
    out->netmask4 = in->netmask4;
    out->gateway4 = in->gateway4;
}

static void copy_char_array_as_string(
    char *out,
    size_t out_size,
    const char *in,
    size_t in_size) {
    if (out_size == 0u) {
        return;
    }
    size_t copy_size = in_size;
    if (copy_size >= out_size) {
        copy_size = out_size - 1u;
    }
    memcpy(out, in, copy_size);
    out[copy_size] = '\0';
}

static h2_pal_result_t map_gpio_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_gpio_irq_event_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_gpio_irq_event_t *pal = (const h2_pal_gpio_irq_event_t *)event->payload;
    h2_runtime_system_event_gpio_irq_t *runtime = &out_payload->gpio_irq;
    runtime->trigger = (h2_runtime_system_gpio_irq_trigger_t)pal->trigger;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_wifi_sta_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_wifi_sta_status_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_wifi_sta_status_t *pal = (const h2_pal_wifi_sta_status_t *)event->payload;
    if (pal->ssid_len > H2_RUNTIME_SYSTEM_WIFI_SSID_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_system_event_wifi_sta_t *runtime = &out_payload->wifi_sta;
    runtime->status = (h2_runtime_system_wifi_sta_status_t)pal->state;
    runtime->ssid_len = pal->ssid_len;
    memcpy(runtime->ssid, pal->ssid, pal->ssid_len);
    runtime->ssid[runtime->ssid_len] = '\0';
    memcpy(runtime->bssid, pal->bssid, sizeof(runtime->bssid));
    runtime->bssid_set = pal->bssid_set;
    runtime->channel = pal->channel;
    runtime->rssi = pal->rssi;
    copy_wifi_ip(&runtime->ip, &pal->ip);
    runtime->ip_valid = pal->ip_valid;
    runtime->disconnect_reason = pal->disconnect_reason;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_wifi_ap_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_wifi_ap_event_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_wifi_ap_event_t *pal = (const h2_pal_wifi_ap_event_t *)event->payload;
    if (pal->status.ssid_len > H2_RUNTIME_SYSTEM_WIFI_SSID_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_system_event_wifi_ap_t *runtime = &out_payload->wifi_ap;
    runtime->status = (h2_runtime_system_wifi_ap_status_t)pal->status.state;
    runtime->ssid_len = pal->status.ssid_len;
    memcpy(runtime->ssid, pal->status.ssid, pal->status.ssid_len);
    runtime->ssid[runtime->ssid_len] = '\0';
    runtime->channel = pal->status.channel;
    runtime->max_clients = pal->status.max_clients;
    runtime->client_count = pal->status.client_count;
    runtime->security = (h2_runtime_system_wifi_security_t)pal->status.security;
    runtime->hidden = pal->status.hidden;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_wifi_client_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_wifi_ap_client_event_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_wifi_ap_client_event_t *pal =
        (const h2_pal_wifi_ap_client_event_t *)event->payload;
    h2_runtime_system_event_wifi_ap_client_t *runtime =
        &out_payload->wifi_ap_client;
    memcpy(runtime->mac, pal->client.mac, sizeof(runtime->mac));
    runtime->rssi = pal->client.rssi;
    copy_wifi_ip(&runtime->lease, &pal->client.lease);
    runtime->lease_valid = pal->client.lease_valid;
    runtime->station_id = pal->client.station_id;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static void copy_ble_addr(
    h2_runtime_system_ble_addr_t *out,
    const h2_pal_ble_addr_t *in) {
    memcpy(out->value, in->value, sizeof(out->value));
    out->type = (h2_runtime_system_ble_addr_type_t)in->type;
}

static h2_pal_result_t map_ble_connection_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_ble_connection_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_ble_connection_t *pal = (const h2_pal_ble_connection_t *)event->payload;
    h2_runtime_system_event_ble_connection_t *runtime =
        &out_payload->ble_connection;
    runtime->conn_handle = pal->conn_handle;
    runtime->role = (h2_runtime_system_ble_role_t)pal->role;
    copy_ble_addr(&runtime->peer_addr, &pal->peer_addr);
    runtime->mtu = pal->mtu;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_ble_advertising_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (no_payload(event)) {
        return H2_PAL_OK;
    }
    if (!payload_is(event, sizeof(h2_pal_ble_adv_set_event_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_ble_adv_set_event_t *pal = event->payload;
    if (pal->set == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_system_event_ble_advertising_set_t *runtime =
        &out_payload->ble_advertising_set;
    runtime->set = pal->set;
    runtime->result = pal->status;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_ble_connection_params_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_ble_connection_params_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_ble_connection_params_t *pal =
        (const h2_pal_ble_connection_params_t *)event->payload;
    h2_runtime_system_event_ble_connection_params_t *runtime =
        &out_payload->ble_connection_params;
    runtime->interval_min_ms = pal->interval_min_ms;
    runtime->interval_max_ms = pal->interval_max_ms;
    runtime->latency = pal->latency;
    runtime->supervision_timeout_ms = pal->supervision_timeout_ms;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_ble_disconnected_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_ble_disconnected_info_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_ble_disconnected_info_t *pal =
        (const h2_pal_ble_disconnected_info_t *)event->payload;
    h2_runtime_system_event_ble_disconnected_t *runtime =
        &out_payload->ble_disconnected;
    runtime->conn_handle = pal->conn_handle;
    copy_ble_addr(&runtime->peer_addr, &pal->peer_addr);
    runtime->reason = pal->reason;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_ble_mtu_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_ble_mtu_info_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_ble_mtu_info_t *pal = (const h2_pal_ble_mtu_info_t *)event->payload;
    h2_runtime_system_event_ble_mtu_t *runtime = &out_payload->ble_mtu;
    runtime->conn_handle = pal->conn_handle;
    runtime->mtu = pal->mtu;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_ble_subscription_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_ble_subscription_state_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_ble_subscription_state_t *pal =
        (const h2_pal_ble_subscription_state_t *)event->payload;
    h2_runtime_system_event_ble_subscription_t *runtime =
        &out_payload->ble_subscription;
    runtime->conn_handle = pal->conn_handle;
    runtime->value_handle = pal->value_handle;
    runtime->mode = (h2_runtime_system_ble_subscribe_mode_t)pal->mode;
    runtime->enabled = pal->enabled;
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_ble_gatt_client_value_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_ble_gatt_client_value_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_ble_gatt_client_value_t *pal =
        (const h2_pal_ble_gatt_client_value_t *)event->payload;
    if (pal->value_len > H2_RUNTIME_SYSTEM_BLE_ATT_MAX_VALUE_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_system_event_ble_gatt_client_value_t *runtime =
        &out_payload->ble_gatt_client_value;
    runtime->conn_handle = pal->conn_handle;
    runtime->attr_handle = pal->attr_handle;
    runtime->value_len = pal->value_len;
    memcpy(runtime->value, pal->value, pal->value_len);
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_modem_status_event(
    const h2_pal_system_event_t *event,
    h2_runtime_event_kind_t kind,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_modem_status_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_modem_status_t *pal = (const h2_pal_modem_status_t *)event->payload;
    if (kind == H2_RUNTIME_SYSTEM_EVENT_MODEM_SIM_CHANGED) {
        h2_runtime_system_event_modem_sim_t *runtime =
            &out_payload->modem_sim;
        runtime->capabilities = pal->capabilities;
        runtime->state = (h2_runtime_system_modem_sim_state_t)pal->sim;
        runtime->rat = (h2_runtime_system_modem_rat_t)pal->rat;
        *out_payload_size = sizeof(*runtime);
        return H2_PAL_OK;
    }
    if (kind == H2_RUNTIME_SYSTEM_EVENT_MODEM_REGISTRATION_CHANGED) {
        h2_runtime_system_event_modem_registration_t *runtime =
            &out_payload->modem_registration;
        runtime->capabilities = pal->capabilities;
        runtime->state = (h2_runtime_system_modem_registration_state_t)pal->registration;
        runtime->rat = (h2_runtime_system_modem_rat_t)pal->rat;
        *out_payload_size = sizeof(*runtime);
        return H2_PAL_OK;
    }
    if (kind == H2_RUNTIME_SYSTEM_EVENT_MODEM_PACKET_CHANGED) {
        h2_runtime_system_event_modem_packet_t *runtime =
            &out_payload->modem_packet;
        runtime->capabilities = pal->capabilities;
        runtime->state = (h2_runtime_system_modem_packet_state_t)pal->packet;
        runtime->rat = (h2_runtime_system_modem_rat_t)pal->rat;
        *out_payload_size = sizeof(*runtime);
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_INVALID_ARG;
}

static h2_pal_result_t map_modem_event(
    const h2_pal_system_event_t *event,
    h2_runtime_event_kind_t kind,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    switch (kind) {
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_READY:
        return no_payload(event) ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG;
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_ERROR: {
        if (!payload_is(event, sizeof(h2_pal_modem_event_t))) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        const h2_pal_modem_event_t *pal = (const h2_pal_modem_event_t *)event->payload;
        h2_runtime_system_event_modem_error_t *runtime =
            &out_payload->modem_error;
        runtime->result = (h2_runtime_system_result_t)pal->result;
        runtime->vendor_code = pal->vendor_code;
        *out_payload_size = sizeof(*runtime);
        return H2_PAL_OK;
    }
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_SIM_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_REGISTRATION_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_PACKET_CHANGED:
        return map_modem_status_event(event, kind, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_SIGNAL_CHANGED: {
        if (!payload_is(event, sizeof(h2_pal_modem_signal_t))) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        const h2_pal_modem_signal_t *pal = (const h2_pal_modem_signal_t *)event->payload;
        h2_runtime_system_event_modem_signal_t *runtime =
            &out_payload->modem_signal;
        runtime->rssi_dbm = pal->rssi_dbm;
        runtime->ber = pal->ber;
        runtime->rat = (h2_runtime_system_modem_rat_t)pal->rat;
        *out_payload_size = sizeof(*runtime);
        return H2_PAL_OK;
    }
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_OPENED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_CLOSED: {
        if (!payload_is(event, sizeof(h2_pal_modem_data_status_t))) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        const h2_pal_modem_data_status_t *pal =
            (const h2_pal_modem_data_status_t *)event->payload;
        h2_runtime_system_event_modem_data_t *runtime =
            &out_payload->modem_data;
        runtime->state = (h2_runtime_system_modem_data_state_t)pal->state;
        runtime->ip4 = pal->ip4;
        runtime->dns1_ip4 = pal->dns1_ip4;
        runtime->dns2_ip4 = pal->dns2_ip4;
        runtime->ip4_valid = pal->ip4_valid;
        runtime->last_error = (h2_runtime_system_result_t)pal->last_error;
        *out_payload_size = sizeof(*runtime);
        return H2_PAL_OK;
    }
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_INCOMING:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_STATE_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_ENDED: {
        if (!payload_is(event, sizeof(h2_pal_modem_call_event_t))) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        const h2_pal_modem_call_event_t *pal =
            (const h2_pal_modem_call_event_t *)event->payload;
        h2_runtime_system_event_modem_call_t *runtime =
            &out_payload->modem_call;
        runtime->call_id = pal->call.call_id;
        runtime->direction = (h2_runtime_system_modem_call_direction_t)pal->call.direction;
        runtime->state = (h2_runtime_system_modem_call_state_t)pal->call.state;
        copy_char_array_as_string(
            runtime->number,
            sizeof(runtime->number),
            pal->call.number,
            sizeof(pal->call.number));
        runtime->end_reason = pal->call.end_reason;
        *out_payload_size = sizeof(*runtime);
        return H2_PAL_OK;
    }
    default:
        return H2_PAL_ERR_INVALID_ARG;
    }
}

static h2_runtime_system_netif_kind_t map_netif_kind(
    h2_pal_netif_kind_t kind) {
    switch (kind) {
    case H2_PAL_NETIF_KIND_LOOPBACK:
        return H2_RUNTIME_SYSTEM_NETIF_KIND_LOOPBACK;
    case H2_PAL_NETIF_KIND_WIFI_STA:
        return H2_RUNTIME_SYSTEM_NETIF_KIND_WIFI_STA;
    case H2_PAL_NETIF_KIND_WIFI_AP:
        return H2_RUNTIME_SYSTEM_NETIF_KIND_WIFI_AP;
    case H2_PAL_NETIF_KIND_MODEM_DATA:
        return H2_RUNTIME_SYSTEM_NETIF_KIND_MODEM_DATA;
    case H2_PAL_NETIF_KIND_ETHERNET:
        return H2_RUNTIME_SYSTEM_NETIF_KIND_ETHERNET;
    case H2_PAL_NETIF_KIND_UNKNOWN:
    default:
        return H2_RUNTIME_SYSTEM_NETIF_KIND_UNKNOWN;
    }
}

static void map_netif_ref(
    h2_runtime_system_netif_ref_t *out,
    const h2_pal_netif_ref_t *in) {
    out->kind = map_netif_kind(in->kind);
    if (in->type == H2_PAL_NETIF_REF_NAME) {
        memcpy(out->name, in->name, sizeof(out->name));
        out->name_valid = 1u;
    } else {
        out->id = in->id;
        out->id_valid = 1u;
    }
}

static h2_pal_result_t map_netif_event(
    const h2_pal_system_event_t *event,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    if (!payload_is(event, sizeof(h2_pal_netif_default_changed_t))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_netif_default_changed_t *pal =
        (const h2_pal_netif_default_changed_t *)event->payload;
    if (!h2_pal_netif_default_changed_is_valid(pal)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_system_event_netif_default_changed_t *runtime =
        &out_payload->netif_default_changed;
    runtime->previous_valid = pal->previous_valid;
    runtime->current_valid = pal->current_valid;
    if (pal->previous_valid != 0u) {
        map_netif_ref(&runtime->previous, &pal->previous);
    }
    if (pal->current_valid != 0u) {
        map_netif_ref(&runtime->current, &pal->current);
    }
    *out_payload_size = sizeof(*runtime);
    return H2_PAL_OK;
}

static h2_pal_result_t map_payload(
    const h2_pal_system_event_t *event,
    h2_runtime_event_kind_t kind,
    h2_runtime_queued_payload_t *out_payload,
    size_t *out_payload_size) {
    *out_payload_size = 0u;
    switch (kind) {
    case H2_RUNTIME_SYSTEM_EVENT_GPIO_IRQ_TRIGGERED:
        return map_gpio_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP:
        return map_wifi_sta_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STOPPED:
        return map_wifi_ap_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED:
    case H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED:
        return map_wifi_client_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STOPPED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STOPPED:
        return no_payload(event) ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG;
    case H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED:
        return map_ble_advertising_event(
            event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTED:
        return map_ble_connection_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTION_UPDATED:
        return map_ble_connection_params_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_BLE_DISCONNECTED:
        return map_ble_disconnected_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_BLE_MTU_CHANGED:
        return map_ble_mtu_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_BLE_SUBSCRIPTION_CHANGED:
        return map_ble_subscription_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_NOTIFICATION:
    case H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_INDICATION:
        return map_ble_gatt_client_value_event(event, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_READY:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_ERROR:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_SIM_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_REGISTRATION_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_PACKET_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_SIGNAL_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_OPENED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_CLOSED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_INCOMING:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_STATE_CHANGED:
    case H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_ENDED:
        return map_modem_event(event, kind, out_payload, out_payload_size);
    case H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED:
        return map_netif_event(event, out_payload, out_payload_size);
    default:
        return H2_PAL_ERR_INVALID_ARG;
    }
}

static int h2_runtime_system_event_handler(void *user, const h2_pal_system_event_t *event) {
    h2_runtime_t *runtime = (h2_runtime_t *)user;
    if (!h2_runtime_ready(runtime) || event == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (atomic_load_explicit(
            &runtime->private_state->system_event_active,
            memory_order_acquire) == 0) {
        return H2_PAL_ERR_CLOSED;
    }

    h2_runtime_event_kind_t kind = system_kind_from_pal(event->type);
    h2_runtime_component_t component = system_component_from_kind(kind);
    if (kind == H2_RUNTIME_EVENT_NONE || component == H2_RUNTIME_COMPONENT_NONE) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_runtime_queued_event_t queued = {
        .kind = kind,
        .component = component,
        .component_id = H2_RUNTIME_COMPONENT_ID_NONE,
        .sequence = h2_runtime_next_sequence(runtime),
        .timestamp_ms = h2_runtime_now_ms(runtime->time),
    };
    h2_pal_result_t rc =
        map_payload(event, kind, &queued.payload, &queued.payload_size);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_runtime_enqueue_event(runtime, &queued);
}

h2_pal_result_t h2_runtime_start_system_events(h2_runtime_t *runtime) {
    if (!h2_runtime_ready(runtime)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_system_event_api_t *api = runtime->system_event;
    if (api == NULL) {
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = h2_pal_system_event_init(api);
    if (rc == H2_PAL_ERR_UNSUPPORTED || rc == H2_PAL_ERR_UNAVAILABLE) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    atomic_store_explicit(
        &runtime->private_state->system_event_active, 1,
        memory_order_release);

    const size_t count = sizeof(s_system_event_types) / sizeof(s_system_event_types[0]);
    for (size_t i = 0u; i < count; ++i) {
        if (runtime->private_state->system_event_subscription_count >= H2_RUNTIME_SYSTEM_EVENT_SUBSCRIPTION_MAX) {
            rc = H2_PAL_ERR_NO_SPACE;
        } else {
            h2_pal_system_event_subscription_t *subscription = NULL;
            rc = h2_pal_system_event_subscribe(
                api,
                s_system_event_types[i],
                h2_runtime_system_event_handler,
                runtime,
                &subscription);
            if (rc == H2_PAL_OK) {
                runtime->private_state->system_event_subscriptions[runtime->private_state->system_event_subscription_count++] =
                    subscription;
            }
        }
        if (rc != H2_PAL_OK) {
            h2_runtime_stop_system_events(runtime);
            return rc;
        }
    }
    return H2_PAL_OK;
}

void h2_runtime_stop_system_events(h2_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }
    const h2_pal_system_event_api_t *api = runtime->system_event;
    int was_active = atomic_exchange_explicit(
        &runtime->private_state->system_event_active, 0,
        memory_order_acq_rel);
    if (api == NULL) {
        return;
    }
    for (size_t i = runtime->private_state->system_event_subscription_count; i > 0u; --i) {
        h2_pal_system_event_unsubscribe(api, runtime->private_state->system_event_subscriptions[i - 1u]);
        runtime->private_state->system_event_subscriptions[i - 1u] = NULL;
    }
    runtime->private_state->system_event_subscription_count = 0u;
    if (was_active != 0) {
        h2_pal_system_event_deinit(api);
    }
}
