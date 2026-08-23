#include "h2_bleikcp_internal.h"

#include <string.h>

#define H2_BLEIKCP_DISCOVERY_MAX 8u

static int h2_bleikcp_client_event(void *user, const h2_pal_system_event_t *event) {
    h2_bleikcp_t *stream = user;
    if (event == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED) {
        if (event->payload_size != sizeof(h2_pal_ble_disconnected_info_t)) return H2_PAL_OK;
        const h2_pal_ble_disconnected_info_t *info = event->payload;
        if (info->conn_handle == stream->conn_handle) {
            h2_bleikcp_stream_mark_closed(stream, H2_PAL_ERR_CLOSED, true);
        }
    } else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION) {
        if (event->payload_size != sizeof(h2_pal_ble_gatt_client_value_t)) return H2_PAL_OK;
        const h2_pal_ble_gatt_client_value_t *value = event->payload;
        if (value->conn_handle == stream->conn_handle &&
            value->attr_handle == stream->tx_value_handle && value->value_len > 0u &&
            value->value_len <= sizeof(value->value)) {
            (void)h2_bleikcp_stream_input(stream, value->value, value->value_len);
        }
    }
    return H2_PAL_OK;
}

static int h2_bleikcp_discover_one(
    h2_bleikcp_t *stream,
    h2_pal_ble_gatt_discovery_kind_t kind,
    const h2_pal_ble_uuid_t *uuid,
    uint16_t start_handle,
    uint16_t end_handle,
    h2_pal_ble_gatt_discovery_entry_t *out) {
    h2_pal_ble_gatt_discovery_entry_t entries[H2_BLEIKCP_DISCOVERY_MAX];
    size_t count = 0u;
    h2_pal_ble_gatt_discovery_request_t request = {
        .kind = kind,
        .uuid_filter = *uuid,
        .start_handle = start_handle,
        .end_handle = end_handle,
    };
    int rc = h2_pal_ble_gatt_discover(
        stream->api.ble, stream->conn_handle, &request, entries,
        H2_BLEIKCP_DISCOVERY_MAX, &count,
        stream->config.value.setup_timeout_ms);
    if (rc != H2_PAL_OK) return rc;
    for (size_t i = 0u; i < count; ++i) {
        if (h2_bleikcp_uuid_equal(&entries[i].uuid, uuid)) {
            *out = entries[i];
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int h2_bleikcp_client_discover(h2_bleikcp_t *stream) {
    h2_pal_ble_gatt_discovery_entry_t service;
    int rc = h2_bleikcp_discover_one(
        stream, H2_PAL_BLE_GATT_DISCOVERY_SERVICE,
        &stream->config.value.service_uuid, 1u, UINT16_MAX, &service);
    if (rc != H2_PAL_OK) return rc;

    h2_pal_ble_gatt_discovery_entry_t tx;
    rc = h2_bleikcp_discover_one(
        stream, H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC,
        &stream->config.value.tx_char_uuid, service.start_handle,
        service.end_handle, &tx);
    if (rc != H2_PAL_OK) return rc;
    if ((tx.properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) == 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    h2_pal_ble_gatt_discovery_entry_t rx;
    rc = h2_bleikcp_discover_one(
        stream, H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC,
        &stream->config.value.rx_char_uuid, service.start_handle,
        service.end_handle, &rx);
    if (rc != H2_PAL_OK) return rc;
    if ((rx.properties & H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP) == 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    static const uint8_t cccd_uuid_data[] = { 0x02u, 0x29u };
    h2_pal_ble_uuid_t cccd_uuid = {
        .data = cccd_uuid_data,
        .len = sizeof(cccd_uuid_data),
    };
    h2_pal_ble_gatt_discovery_entry_t cccd;
    rc = h2_bleikcp_discover_one(
        stream, H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR, &cccd_uuid,
        (uint16_t)(tx.value_handle + 1u), service.end_handle, &cccd);
    if (rc != H2_PAL_OK) return rc;
    stream->tx_value_handle = tx.value_handle;
    stream->tx_cccd_handle = cccd.value_handle;
    stream->rx_value_handle = rx.value_handle;
    return H2_PAL_OK;
}

int h2_bleikcp_client_open(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_config_t *config,
    uint16_t conn_handle,
    uint16_t negotiated_att_mtu,
    h2_bleikcp_t **out_stream) {
    if (out_stream == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_stream = NULL;
    if (api == NULL || api->ble == NULL ||
        conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_bleikcp_resolved_config_t resolved;
    int rc = h2_bleikcp_resolve_config(api, config, &resolved);
    if (rc != H2_PAL_OK) return rc;
    h2_bleikcp_t *stream = NULL;
    rc = h2_bleikcp_stream_create(
        api, &resolved, H2_BLEIKCP_ROLE_CLIENT, conn_handle,
        negotiated_att_mtu, false, &stream);
    if (rc != H2_PAL_OK) return rc;

    rc = h2_bleikcp_client_discover(stream);
    if (rc != H2_PAL_OK) goto fail;
    rc = h2_pal_system_event_subscribe(
        api->system_event, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
        h2_bleikcp_client_event, stream, &stream->subscriptions[0]);
    if (rc != H2_PAL_OK) goto fail;
    rc = h2_pal_system_event_subscribe(
        api->system_event, H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
        h2_bleikcp_client_event, stream, &stream->subscriptions[1]);
    if (rc != H2_PAL_OK) goto fail;
    h2_pal_ble_gatt_subscribe_t subscribe = {
        .value_handle = stream->tx_value_handle,
        .cccd_handle = stream->tx_cccd_handle,
        .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
        .enable = true,
    };
    rc = h2_pal_ble_gatt_subscribe(
        api->ble, conn_handle, &subscribe, resolved.value.setup_timeout_ms);
    if (rc != H2_PAL_OK) goto fail;
    rc = h2_bleikcp_stream_start(stream);
    if (rc != H2_PAL_OK) {
        subscribe.enable = false;
        (void)h2_pal_ble_gatt_subscribe(
            api->ble, conn_handle, &subscribe, resolved.value.setup_timeout_ms);
        goto fail;
    }
    *out_stream = stream;
    return H2_PAL_OK;

fail:
    for (size_t i = 0u; i < H2_BLEIKCP_SUBSCRIPTION_COUNT; ++i) {
        h2_pal_system_event_unsubscribe(api->system_event, stream->subscriptions[i]);
        stream->subscriptions[i] = NULL;
    }
    (void)h2_bleikcp_stream_destroy(stream);
    return rc;
}
