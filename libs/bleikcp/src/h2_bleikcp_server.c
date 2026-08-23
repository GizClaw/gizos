#include "h2_bleikcp_internal.h"

#include <string.h>

#define H2_BLEIKCP_SERVER_SUBSCRIPTION_COUNT 4u

struct h2_bleikcp_server {
    h2_bleikcp_api_t api;
    h2_bleikcp_resolved_config_t config;
    h2_bleikcp_server_handler_fn handler;
    void *handler_user;
    h2_pal_mutex_t *mutex;
    h2_pal_cond_t *cond;
    h2_pal_task_t *dispatch_task;
    h2_pal_system_event_subscription_t *subscriptions[H2_BLEIKCP_SERVER_SUBSCRIPTION_COUNT];
    h2_pal_ble_gatt_characteristic_t characteristics[2];
    h2_pal_ble_gatt_service_t service;
    uint16_t service_handle;
    uint16_t tx_value_handle;
    uint16_t tx_cccd_handle;
    uint16_t rx_value_handle;
    uint16_t conn_handle;
    uint16_t att_mtu;
    bool tx_subscribed;
    bool dispatch_pending;
    bool connected_event_pending;
    bool protocol_error_pending;
    bool handling;
    bool closing;
    bool disconnected;
    h2_bleikcp_t *active_stream;
};

static void h2_bleikcp_server_signal_if_ready(h2_bleikcp_server_t *server) {
    if (!server->closing && !server->handling && !server->dispatch_pending &&
        server->conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
        server->att_mtu >= H2_BLEIKCP_MIN_ATT_MTU && server->tx_subscribed) {
        server->dispatch_pending = true;
        (void)h2_pal_cond_broadcast(server->api.sync, server->cond);
    }
}

static int h2_bleikcp_server_event(void *user, const h2_pal_system_event_t *event) {
    h2_bleikcp_server_t *server = user;
    if (event == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)h2_pal_mutex_lock(server->api.sync, server->mutex);
    switch (event->type) {
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED:
        if (event->payload_size == sizeof(h2_pal_ble_connection_t)) {
            const h2_pal_ble_connection_t *connection = event->payload;
            if (connection->role == H2_PAL_BLE_ROLE_PERIPHERAL &&
                server->conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
                server->conn_handle = connection->conn_handle;
                server->att_mtu = connection->mtu;
                server->tx_subscribed = false;
                server->disconnected = false;
                server->connected_event_pending = true;
                (void)h2_pal_cond_broadcast(server->api.sync, server->cond);
                h2_bleikcp_server_signal_if_ready(server);
            }
        }
        break;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED:
        if (event->payload_size == sizeof(h2_pal_ble_mtu_info_t)) {
            const h2_pal_ble_mtu_info_t *info = event->payload;
            if (info->conn_handle == server->conn_handle) {
                server->att_mtu = info->mtu;
                h2_bleikcp_server_signal_if_ready(server);
            }
        }
        break;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED:
        if (event->payload_size == sizeof(h2_pal_ble_subscription_state_t)) {
            const h2_pal_ble_subscription_state_t *state = event->payload;
            if (state->conn_handle == server->conn_handle &&
                state->value_handle == server->tx_value_handle) {
                if (!state->enabled ||
                    state->mode != H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY) {
                    server->tx_subscribed = false;
                    server->dispatch_pending = false;
                    if (server->active_stream != NULL) {
                        h2_bleikcp_stream_mark_closed(
                            server->active_stream, H2_PAL_ERR_CLOSED, false);
                    }
                } else {
                    server->tx_subscribed = true;
                }
                if (server->tx_subscribed && server->att_mtu < H2_BLEIKCP_MIN_ATT_MTU) {
                    server->protocol_error_pending = true;
                    (void)h2_pal_cond_broadcast(server->api.sync, server->cond);
                }
                h2_bleikcp_server_signal_if_ready(server);
            }
        }
        break;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED:
        if (event->payload_size == sizeof(h2_pal_ble_disconnected_info_t)) {
            const h2_pal_ble_disconnected_info_t *info = event->payload;
            if (info->conn_handle == server->conn_handle) {
                server->disconnected = true;
                server->dispatch_pending = false;
                server->connected_event_pending = false;
                server->protocol_error_pending = false;
                server->tx_subscribed = false;
                if (server->active_stream != NULL) {
                    h2_bleikcp_stream_mark_closed(
                        server->active_stream, H2_PAL_ERR_CLOSED, true);
                } else if (!server->handling) {
                    server->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
                    server->att_mtu = 0u;
                }
                (void)h2_pal_cond_broadcast(server->api.sync, server->cond);
            }
        }
        break;
    default:
        break;
    }
    (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bleikcp_server_rx_write(
    void *user,
    const h2_pal_ble_gatt_access_t *access,
    const uint8_t *data,
    size_t len) {
    h2_bleikcp_server_t *server = user;
    if (access == NULL || (len > 0u && data == NULL)) return H2_PAL_ERR_INVALID_ARG;
    (void)h2_pal_mutex_lock(server->api.sync, server->mutex);
    h2_bleikcp_t *stream = access->conn_handle == server->conn_handle
                                 ? server->active_stream
                                 : NULL;
    int rc = stream != NULL
                 ? h2_bleikcp_stream_input(stream, data, len)
                 : H2_PAL_ERR_WOULD_BLOCK;
    (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
    return rc;
}

static void h2_bleikcp_server_dispatch(void *ctx) {
    h2_bleikcp_server_t *server = ctx;
    for (;;) {
        (void)h2_pal_mutex_lock(server->api.sync, server->mutex);
        while (!server->dispatch_pending && !server->connected_event_pending &&
               !server->protocol_error_pending && !server->closing) {
            (void)h2_pal_cond_wait(
                server->api.sync, server->cond, server->mutex,
                H2_PAL_SYNC_WAIT_FOREVER);
        }
        if (server->closing) {
            (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
            return;
        }
        bool emit_connected = server->connected_event_pending;
        bool emit_protocol_error = server->protocol_error_pending;
        uint16_t event_conn_handle = server->conn_handle;
        server->connected_event_pending = false;
        server->protocol_error_pending = false;
        if (emit_connected || emit_protocol_error) {
            (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
            if (server->config.value.on_event != NULL) {
                if (emit_connected) {
                    server->config.value.on_event(
                        server->config.value.user, NULL,
                        H2_BLEIKCP_EVENT_CONNECTED, event_conn_handle, H2_PAL_OK);
                }
                if (emit_protocol_error) {
                    server->config.value.on_event(
                        server->config.value.user, NULL,
                        H2_BLEIKCP_EVENT_PROTOCOL_ERROR, event_conn_handle,
                        H2_PAL_ERR_UNSUPPORTED);
                }
            }
            continue;
        }
        uint16_t conn_handle = server->conn_handle;
        uint16_t att_mtu = server->att_mtu;
        server->dispatch_pending = false;
        server->handling = true;
        (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);

        h2_bleikcp_t *stream = NULL;
        int rc = h2_bleikcp_stream_create(
            &server->api, &server->config, H2_BLEIKCP_ROLE_SERVER,
            conn_handle, att_mtu, true, &stream);
        if (rc == H2_PAL_OK) {
            stream->tx_value_handle = server->tx_value_handle;
            stream->tx_cccd_handle = server->tx_cccd_handle;
            stream->rx_value_handle = server->rx_value_handle;
            rc = h2_bleikcp_stream_start(stream);
        }
        if (rc == H2_PAL_OK) {
            bool invoke_handler = false;
            (void)h2_pal_mutex_lock(server->api.sync, server->mutex);
            server->active_stream = stream;
            if (!server->closing && server->conn_handle == conn_handle && !server->disconnected) {
                invoke_handler = true;
            } else {
                h2_bleikcp_stream_mark_closed(stream, H2_PAL_ERR_CLOSED, true);
            }
            (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
            if (invoke_handler) {
                rc = server->handler(server->handler_user, stream, conn_handle);
                if (rc != H2_PAL_OK && rc != H2_PAL_ERR_CLOSED) {
                    h2_bleikcp_stream_mark_closed(stream, rc, false);
                }
            }
        }

        (void)h2_pal_mutex_lock(server->api.sync, server->mutex);
        bool should_disconnect = !server->disconnected &&
                                 server->conn_handle == conn_handle;
        (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
        rc = h2_bleikcp_stream_join(stream);
        if (should_disconnect) {
            (void)h2_pal_ble_disconnect(server->api.ble, conn_handle);
        }

        (void)h2_pal_mutex_lock(server->api.sync, server->mutex);
        if (rc != H2_PAL_OK) {
            server->closing = true;
            server->handling = false;
            (void)h2_pal_cond_broadcast(server->api.sync, server->cond);
            (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
            return;
        }
        server->active_stream = NULL;
        server->handling = false;
        server->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        server->att_mtu = 0u;
        server->tx_subscribed = false;
        server->disconnected = false;
        (void)h2_pal_cond_broadcast(server->api.sync, server->cond);
        (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
        (void)h2_bleikcp_stream_destroy(stream);
    }
}

static int h2_bleikcp_server_subscribe_events(h2_bleikcp_server_t *server) {
    static const h2_pal_system_event_type_t event_types[] = {
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
    };
    for (size_t i = 0u; i < H2_BLEIKCP_SERVER_SUBSCRIPTION_COUNT; ++i) {
        int rc = h2_pal_system_event_subscribe(
            server->api.system_event, event_types[i], h2_bleikcp_server_event,
            server, &server->subscriptions[i]);
        if (rc != H2_PAL_OK) return rc;
    }
    return H2_PAL_OK;
}

int h2_bleikcp_server_open(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_config_t *config,
    h2_bleikcp_server_handler_fn handler,
    void *handler_user,
    h2_bleikcp_server_t **out_server) {
    if (api == NULL || out_server == NULL || handler == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_server = NULL;
    if (api->ble == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_bleikcp_resolved_config_t resolved;
    int rc = h2_bleikcp_resolve_config(api, config, &resolved);
    if (rc != H2_PAL_OK) return rc;
    h2_bleikcp_server_t *server = h2_pal_mem_alloc(api->allocator, sizeof(*server));
    if (server == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(server, 0, sizeof(*server));
    server->api = *api;
    server->config = resolved;
    server->config.value.service_uuid.data = server->config.service_uuid;
    server->config.value.tx_char_uuid.data = server->config.tx_uuid;
    server->config.value.rx_char_uuid.data = server->config.rx_uuid;
    server->handler = handler;
    server->handler_user = handler_user;
    server->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    server->service_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    server->tx_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    server->tx_cccd_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    server->rx_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;

    h2_pal_mutex_config_t mutex_config = {
        .name = "bleikcp/server",
        .allocator = api->allocator,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    rc = h2_pal_mutex_create(api->sync, &mutex_config, &server->mutex);
    if (rc != H2_PAL_OK) goto fail;
    h2_pal_cond_config_t cond_config = {
        .name = "bleikcp/server",
        .allocator = api->allocator,
    };
    rc = h2_pal_cond_create(api->sync, &cond_config, &server->cond);
    if (rc != H2_PAL_OK) goto fail;
    rc = h2_bleikcp_server_subscribe_events(server);
    if (rc != H2_PAL_OK) goto fail;

    server->characteristics[0] = (h2_pal_ble_gatt_characteristic_t){
        .uuid = server->config.value.tx_char_uuid,
        .properties = H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
        .permissions = 0u,
        .max_value_len = server->config.value.max_datagram_len,
        .user = server,
        .out_value_handle = &server->tx_value_handle,
        .out_cccd_handle = &server->tx_cccd_handle,
    };
    server->characteristics[1] = (h2_pal_ble_gatt_characteristic_t){
        .uuid = server->config.value.rx_char_uuid,
        .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE |
                      H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP,
        .permissions = H2_PAL_BLE_GATT_PERMISSION_WRITE,
        .max_value_len = server->config.value.max_datagram_len,
        .write = h2_bleikcp_server_rx_write,
        .user = server,
        .out_value_handle = &server->rx_value_handle,
    };
    server->service = (h2_pal_ble_gatt_service_t){
        .uuid = server->config.value.service_uuid,
        .primary = true,
        .characteristics = server->characteristics,
        .characteristic_count = 2u,
        .out_service_handle = &server->service_handle,
    };
    rc = h2_pal_task_start(
        api->task, &server->config.value.server_task_options,
        h2_bleikcp_server_dispatch, server, &server->dispatch_task);
    if (rc != H2_PAL_OK) goto fail;
    rc = h2_pal_ble_register_gatt_services(api->ble, &server->service, 1u);
    if (rc != H2_PAL_OK) goto fail;
    *out_server = server;
    return H2_PAL_OK;

fail:
    if (server->dispatch_task != NULL) {
        (void)h2_pal_mutex_lock(api->sync, server->mutex);
        server->closing = true;
        (void)h2_pal_cond_broadcast(api->sync, server->cond);
        (void)h2_pal_mutex_unlock(api->sync, server->mutex);
        int join_rc = h2_pal_task_join(api->task, server->dispatch_task);
        if (join_rc != H2_PAL_OK) return join_rc;
        server->dispatch_task = NULL;
    }
    for (size_t i = 0u; i < H2_BLEIKCP_SERVER_SUBSCRIPTION_COUNT; ++i) {
        h2_pal_system_event_unsubscribe(api->system_event, server->subscriptions[i]);
    }
    if (server->cond != NULL) (void)h2_pal_cond_destroy(api->sync, server->cond);
    if (server->mutex != NULL) (void)h2_pal_mutex_destroy(api->sync, server->mutex);
    h2_pal_mem_free(api->allocator, server);
    return rc;
}

int h2_bleikcp_server_close(h2_bleikcp_server_t *server) {
    if (server == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)h2_pal_mutex_lock(server->api.sync, server->mutex);
    server->closing = true;
    server->dispatch_pending = false;
    if (server->active_stream != NULL) {
        h2_bleikcp_stream_mark_closed(
            server->active_stream, H2_PAL_ERR_CLOSED, false);
    }
    (void)h2_pal_cond_broadcast(server->api.sync, server->cond);
    (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
    if (server->dispatch_task != NULL) {
        int rc = h2_pal_task_join(server->api.task, server->dispatch_task);
        if (rc != H2_PAL_OK) return rc;
        server->dispatch_task = NULL;
    }
    if (server->active_stream != NULL) {
        h2_bleikcp_t *stream = server->active_stream;
        int rc = h2_bleikcp_stream_join(stream);
        if (rc != H2_PAL_OK) return rc;
        (void)h2_pal_mutex_lock(server->api.sync, server->mutex);
        if (server->active_stream == stream) server->active_stream = NULL;
        (void)h2_pal_mutex_unlock(server->api.sync, server->mutex);
        (void)h2_bleikcp_stream_destroy(stream);
    }
    int rc = h2_pal_ble_unregister_gatt_services(server->api.ble);
    if (rc != H2_PAL_OK) return rc;
    for (size_t i = 0u; i < H2_BLEIKCP_SERVER_SUBSCRIPTION_COUNT; ++i) {
        h2_pal_system_event_unsubscribe(
            server->api.system_event, server->subscriptions[i]);
        server->subscriptions[i] = NULL;
    }
    (void)h2_pal_cond_destroy(server->api.sync, server->cond);
    (void)h2_pal_mutex_destroy(server->api.sync, server->mutex);
    h2_pal_mem_free(server->api.allocator, server);
    return rc;
}
