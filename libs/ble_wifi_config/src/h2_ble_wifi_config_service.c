#include "h2_ble_wifi_config_internal.h"
#include "h2_ble_wifi_config_task_names.h"

#include <string.h>

/*
 * 128-bit UUIDs in ATT byte order, which is the reverse of the printed form.
 * Printed: 0000a1xx-0000-1000-8000-00805f9b34fb.
 */
#define H2_BLE_WIFI_CONFIG_UUID_TAIL \
    0xfbu, 0x34u, 0x9bu, 0x5fu, 0x80u, 0x00u, 0x00u, 0x80u, \
    0x00u, 0x10u, 0x00u, 0x00u

const uint8_t h2_ble_wifi_config_default_service_uuid[16] = {
    H2_BLE_WIFI_CONFIG_UUID_TAIL, 0x00u, 0xa1u, 0x00u, 0x00u,
};
const uint8_t h2_ble_wifi_config_default_command_uuid[16] = {
    H2_BLE_WIFI_CONFIG_UUID_TAIL, 0x01u, 0xa1u, 0x00u, 0x00u,
};
const uint8_t h2_ble_wifi_config_default_scan_uuid[16] = {
    H2_BLE_WIFI_CONFIG_UUID_TAIL, 0x02u, 0xa1u, 0x00u, 0x00u,
};
const uint8_t h2_ble_wifi_config_default_provision_uuid[16] = {
    H2_BLE_WIFI_CONFIG_UUID_TAIL, 0x03u, 0xa1u, 0x00u, 0x00u,
};

static bool h2_ble_wifi_config_api_valid(const h2_ble_wifi_config_api_t *api) {
    return api != NULL && api->ble != NULL && api->wifi_sta != NULL &&
           api->task != NULL && api->sync != NULL &&
           api->system_event != NULL && api->allocator != NULL;
}

static int h2_ble_wifi_config_copy_uuid(
    h2_pal_ble_uuid_t source,
    const uint8_t *default_data,
    uint8_t *storage,
    h2_pal_ble_uuid_t *out) {
    if (source.len == 0u) {
        source.data = default_data;
        source.len = H2_BLE_WIFI_CONFIG_UUID_MAX_LEN;
    }
    if (source.data == NULL ||
        (source.len != 2u && source.len != 4u && source.len != 16u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(storage, source.data, source.len);
    out->data = storage;
    out->len = source.len;
    return H2_PAL_OK;
}

static int h2_ble_wifi_config_resolve(h2_ble_wifi_config_t *service) {
    h2_ble_wifi_config_config_t *config = &service->config;
    int rc = h2_ble_wifi_config_copy_uuid(
        config->service_uuid, h2_ble_wifi_config_default_service_uuid,
        service->service_uuid, &config->service_uuid);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_ble_wifi_config_copy_uuid(
        config->command_char_uuid, h2_ble_wifi_config_default_command_uuid,
        service->command_uuid, &config->command_char_uuid);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_ble_wifi_config_copy_uuid(
        config->scan_char_uuid, h2_ble_wifi_config_default_scan_uuid,
        service->scan_uuid, &config->scan_char_uuid);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_ble_wifi_config_copy_uuid(
        config->provision_char_uuid, h2_ble_wifi_config_default_provision_uuid,
        service->provision_uuid, &config->provision_char_uuid);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    if (config->min_att_mtu == 0u) {
        config->min_att_mtu = H2_BLE_WIFI_CONFIG_MIN_ATT_MTU;
    }
    if (config->min_att_mtu < H2_BLE_WIFI_CONFIG_MIN_ATT_MTU) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->scan_timeout_ms == 0u) {
        config->scan_timeout_ms = H2_BLE_WIFI_CONFIG_DEFAULT_SCAN_TIMEOUT_MS;
    }
    if (config->connect_timeout_ms == 0u) {
        config->connect_timeout_ms =
            H2_BLE_WIFI_CONFIG_DEFAULT_CONNECT_TIMEOUT_MS;
    }
    config->worker_task_options.name = h2_ble_wifi_config_worker_task_name;
    if (config->worker_task_options.min_stack_size <
        H2_BLE_WIFI_CONFIG_MIN_STACK_SIZE) {
        config->worker_task_options.min_stack_size =
            H2_BLE_WIFI_CONFIG_MIN_STACK_SIZE;
    }
    return H2_PAL_OK;
}

static void h2_ble_wifi_config_lock(h2_ble_wifi_config_t *service) {
    (void)h2_pal_mutex_lock(service->api.sync, service->mutex);
}

static void h2_ble_wifi_config_unlock(h2_ble_wifi_config_t *service) {
    (void)h2_pal_mutex_unlock(service->api.sync, service->mutex);
}

/* The caller must hold the mutex. */
static void h2_ble_wifi_config_post_event_locked(
    h2_ble_wifi_config_t *service,
    h2_ble_wifi_config_event_t event,
    uint16_t conn_handle,
    int status) {
    if (service->event_count == H2_BLE_WIFI_CONFIG_EVENT_QUEUE_LEN) {
        service->event_head =
            (service->event_head + 1u) % H2_BLE_WIFI_CONFIG_EVENT_QUEUE_LEN;
        service->event_count--;
    }
    size_t tail = (service->event_head + service->event_count) %
                  H2_BLE_WIFI_CONFIG_EVENT_QUEUE_LEN;
    service->events[tail].event = event;
    service->events[tail].conn_handle = conn_handle;
    service->events[tail].status = status;
    service->event_count++;
    (void)h2_pal_cond_broadcast(service->api.sync, service->cond);
}

static void h2_ble_wifi_config_emit(
    h2_ble_wifi_config_t *service,
    const h2_ble_wifi_config_pending_event_t *event) {
    if (service->config.on_event == NULL) {
        return;
    }
    service->config.on_event(
        service->config.user, service, event->event, event->conn_handle,
        event->status);
}

/**
 * Send one notification on a subscribed characteristic.
 *
 * @p peer is the connection that asked for this work. Wi-Fi work outlives the
 * ATT write that started it, so a frame is dropped rather than delivered to
 * whichever peer happens to be connected now: otherwise a peer that connects
 * and subscribes mid-scan would receive the previous peer's access points or
 * provisioning result.
 *
 * The identity check and the send happen in one critical section. Releasing
 * the mutex in between would let a disconnect and a reconnect that reuses the
 * numeric connection handle land between them, and the send would then reach
 * the new peer. The cost is that a GATT write callback or a BLE system event
 * can block for the length of one notification; the BLE Host must therefore
 * not invoke those callbacks from inside h2_pal_ble_notify() on the calling
 * task, which no supported provider does.
 *
 * Returns H2_PAL_ERR_INVALID_STATE when that connection went away or the peer
 * is not subscribed, so a scan in flight can stop instead of failing every
 * remaining frame.
 */
static int h2_ble_wifi_config_notify(
    h2_ble_wifi_config_t *service,
    bool scan_channel,
    h2_ble_wifi_config_peer_t peer,
    const uint8_t *data,
    size_t len) {
    uint16_t conn_handle = peer.conn_handle;
    h2_ble_wifi_config_lock(service);
    bool current = conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE &&
                   conn_handle == service->conn_handle &&
                   peer.generation == service->conn_generation;
    uint16_t attr_handle = scan_channel ? service->scan_value_handle
                                        : service->provision_value_handle;
    bool subscribed = scan_channel ? service->scan_subscribed
                                   : service->provision_subscribed;
    if (!current || !subscribed ||
        attr_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE) {
        h2_ble_wifi_config_unlock(service);
        return H2_PAL_ERR_INVALID_STATE;
    }
    int rc = h2_pal_ble_notify(service->api.ble, conn_handle, attr_handle, data, len);
    if (rc != H2_PAL_OK) {
        service->stats.notify_failures++;
    }
    h2_ble_wifi_config_unlock(service);
    return rc;
}

static void h2_ble_wifi_config_pause_advertising(h2_ble_wifi_config_t *service) {
    h2_ble_wifi_config_lock(service);
    bool pause = !service->config.keep_advertising_during_wifi &&
                 service->adv_started && !service->adv_paused;
    if (pause) {
        service->adv_paused = true;
    }
    h2_ble_wifi_config_unlock(service);
    if (pause) {
        (void)h2_pal_ble_stop_advertising(service->api.ble);
    }
}

static void h2_ble_wifi_config_resume_advertising(h2_ble_wifi_config_t *service) {
    h2_pal_ble_adv_params_t params;
    h2_ble_wifi_config_lock(service);
    bool resume = service->adv_started && service->adv_paused && !service->closing;
    params = service->adv_params;
    if (resume) {
        service->adv_paused = false;
    }
    h2_ble_wifi_config_unlock(service);
    if (resume) {
        (void)h2_pal_ble_start_advertising(service->api.ble, &params);
    }
}

typedef struct h2_ble_wifi_config_scan_context {
    h2_ble_wifi_config_t *service;
    /** Connection that wrote the start-scan command. */
    h2_ble_wifi_config_peer_t peer;
} h2_ble_wifi_config_scan_context_t;

static bool h2_ble_wifi_config_on_scan_result(
    void *user,
    const h2_pal_wifi_scan_entry_t *entry) {
    h2_ble_wifi_config_scan_context_t *context = user;
    h2_ble_wifi_config_t *service = context->service;
    h2_ble_wifi_config_ap_t ap;
    if (h2_ble_wifi_config_ap_from_scan_entry(entry, &ap) != H2_PAL_OK) {
        h2_ble_wifi_config_lock(service);
        service->stats.aps_dropped++;
        bool stop = service->scan_stop_requested || service->closing;
        h2_ble_wifi_config_unlock(service);
        return stop;
    }
    uint8_t frame[H2_BLE_WIFI_CONFIG_SCAN_FRAME_MAX_LEN];
    size_t frame_len = 0u;
    if (h2_ble_wifi_config_encode_ap(&ap, frame, sizeof(frame), &frame_len) !=
        H2_PAL_OK) {
        h2_ble_wifi_config_lock(service);
        service->stats.aps_dropped++;
        bool stop = service->scan_stop_requested || service->closing;
        h2_ble_wifi_config_unlock(service);
        return stop;
    }
    /* One access point per notification: the application renders as it goes. */
    int rc = h2_ble_wifi_config_notify(
        service, true, context->peer, frame, frame_len);
    h2_ble_wifi_config_lock(service);
    if (rc == H2_PAL_OK) {
        service->stats.aps_reported++;
    }
    bool stop = service->scan_stop_requested || service->closing ||
                rc == H2_PAL_ERR_INVALID_STATE;
    h2_ble_wifi_config_unlock(service);
    return stop;
}

static void h2_ble_wifi_config_run_scan(
    h2_ble_wifi_config_t *service,
    h2_ble_wifi_config_peer_t peer) {
    uint16_t conn_handle = peer.conn_handle;
    h2_ble_wifi_config_lock(service);
    uint32_t timeout_ms = service->config.scan_timeout_ms;
    service->stats.scans_started++;
    h2_ble_wifi_config_unlock(service);

    h2_ble_wifi_config_pending_event_t started = {
        .event = H2_BLE_WIFI_CONFIG_EVENT_SCAN_STARTED,
        .conn_handle = conn_handle,
        .status = H2_PAL_OK,
    };
    h2_ble_wifi_config_emit(service, &started);

    h2_ble_wifi_config_scan_context_t context = {
        .service = service,
        .peer = peer,
    };
    h2_ble_wifi_config_pause_advertising(service);
    int rc = h2_pal_wifi_sta_scan(
        service->api.wifi_sta, NULL, h2_ble_wifi_config_on_scan_result, &context,
        timeout_ms);
    h2_ble_wifi_config_resume_advertising(service);

    uint8_t frame[H2_BLE_WIFI_CONFIG_SCAN_STATUS_FRAME_LEN];
    size_t frame_len = 0u;
    if (h2_ble_wifi_config_encode_scan_status(
            rc == H2_PAL_OK ? H2_BLE_WIFI_CONFIG_SCAN_FRAME_FINISHED
                            : H2_BLE_WIFI_CONFIG_SCAN_FRAME_ERROR,
            frame, sizeof(frame), &frame_len) == H2_PAL_OK) {
        (void)h2_ble_wifi_config_notify(service, true, peer, frame, frame_len);
    }

    h2_ble_wifi_config_pending_event_t finished = {
        .event = H2_BLE_WIFI_CONFIG_EVENT_SCAN_FINISHED,
        .conn_handle = conn_handle,
        .status = rc,
    };
    h2_ble_wifi_config_emit(service, &finished);
}

typedef struct h2_ble_wifi_config_ap_probe {
    const h2_ble_wifi_config_credentials_t *credentials;
    bool found;
} h2_ble_wifi_config_ap_probe_t;

static bool h2_ble_wifi_config_on_probe_result(
    void *user,
    const h2_pal_wifi_scan_entry_t *entry) {
    h2_ble_wifi_config_ap_probe_t *probe = user;
    if (entry == NULL) {
        return false;
    }
    if (entry->ssid_len == probe->credentials->ssid_len &&
        memcmp(entry->ssid, probe->credentials->ssid, entry->ssid_len) == 0) {
        probe->found = true;
        return true;
    }
    return false;
}

/**
 * Look for the requested SSID before connecting.
 *
 * A targeted scan separates "no such access point" from "wrong passphrase"
 * without reading a platform-specific disconnect reason. A scan that fails
 * outright reports found, so a broken scan never masquerades as a missing
 * network.
 */
static bool h2_ble_wifi_config_ap_present(
    h2_ble_wifi_config_t *service,
    const h2_ble_wifi_config_credentials_t *credentials) {
    h2_pal_wifi_scan_request_t request;
    memset(&request, 0, sizeof(request));
    memcpy(request.ssid, credentials->ssid, credentials->ssid_len);
    request.ssid_len = credentials->ssid_len;
    h2_ble_wifi_config_ap_probe_t probe = {
        .credentials = credentials,
        .found = false,
    };
    int rc = h2_pal_wifi_sta_scan(
        service->api.wifi_sta, &request, h2_ble_wifi_config_on_probe_result,
        &probe, service->config.scan_timeout_ms);
    if (rc != H2_PAL_OK) {
        return true;
    }
    return probe.found;
}

static int h2_ble_wifi_config_connect(
    h2_ble_wifi_config_t *service,
    const h2_ble_wifi_config_credentials_t *credentials,
    h2_ble_wifi_config_reason_t *out_reason) {
    *out_reason = H2_BLE_WIFI_CONFIG_REASON_NONE;
    if (service->config.connect != NULL) {
        int rc = service->config.connect(
            service->config.user, credentials, out_reason);
        if (rc != H2_PAL_OK && *out_reason == H2_BLE_WIFI_CONFIG_REASON_NONE) {
            *out_reason = H2_BLE_WIFI_CONFIG_REASON_UNKNOWN;
        }
        return rc;
    }

    if (!service->config.skip_ap_verification_before_connect &&
        !h2_ble_wifi_config_ap_present(service, credentials)) {
        *out_reason = H2_BLE_WIFI_CONFIG_REASON_AP_NOT_FOUND;
        return H2_PAL_ERR_NOT_FOUND;
    }

    h2_pal_wifi_sta_config_t sta_config;
    memset(&sta_config, 0, sizeof(sta_config));
    memcpy(sta_config.ssid, credentials->ssid, credentials->ssid_len);
    sta_config.ssid_len = credentials->ssid_len;
    memcpy(sta_config.password, credentials->password, credentials->password_len);
    sta_config.password_len = credentials->password_len;

    int rc = h2_pal_wifi_sta_connect(
        service->api.wifi_sta, &sta_config, service->config.connect_timeout_ms);

    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));
    bool status_valid =
        h2_pal_wifi_sta_get_status(service->api.wifi_sta, &status) == H2_PAL_OK;
    if (rc == H2_PAL_OK) {
        /* Association alone is not provisioning: the station needs a lease. */
        if (!status_valid || status.ip_valid != 0u) {
            return H2_PAL_OK;
        }
        *out_reason = H2_BLE_WIFI_CONFIG_REASON_DHCP_FAILED;
        return H2_PAL_ERR_UNAVAILABLE;
    }
    const h2_pal_wifi_sta_status_t *status_arg = status_valid ? &status : NULL;
    *out_reason = service->config.map_reason != NULL
                      ? service->config.map_reason(
                            service->config.user, rc, status_arg)
                      : h2_ble_wifi_config_default_reason(rc, status_arg);
    return rc;
}

static void h2_ble_wifi_config_send_result(
    h2_ble_wifi_config_t *service,
    h2_ble_wifi_config_peer_t peer,
    h2_ble_wifi_config_status_t status,
    h2_ble_wifi_config_reason_t reason) {
    uint8_t frame[H2_BLE_WIFI_CONFIG_RESULT_FRAME_LEN];
    size_t frame_len = 0u;
    if (h2_ble_wifi_config_encode_result(
            status, reason, frame, sizeof(frame), &frame_len) != H2_PAL_OK) {
        return;
    }
    (void)h2_ble_wifi_config_notify(service, false, peer, frame, frame_len);
}

static void h2_ble_wifi_config_run_provision(
    h2_ble_wifi_config_t *service,
    const h2_ble_wifi_config_credentials_t *credentials,
    h2_ble_wifi_config_peer_t peer) {
    uint16_t conn_handle = peer.conn_handle;
    h2_ble_wifi_config_pending_event_t received = {
        .event = H2_BLE_WIFI_CONFIG_EVENT_CREDENTIALS_RECEIVED,
        .conn_handle = conn_handle,
        .status = H2_PAL_OK,
    };
    h2_ble_wifi_config_emit(service, &received);

    h2_ble_wifi_config_reason_t reason = H2_BLE_WIFI_CONFIG_REASON_NONE;
    h2_ble_wifi_config_pause_advertising(service);
    int rc = h2_ble_wifi_config_connect(service, credentials, &reason);
    h2_ble_wifi_config_resume_advertising(service);

    h2_ble_wifi_config_lock(service);
    service->stats.provision_attempts++;
    if (rc != H2_PAL_OK) {
        service->stats.provision_failures++;
    }
    h2_ble_wifi_config_unlock(service);

    /* The application waits for this frame before leaving its progress page. */
    h2_ble_wifi_config_send_result(
        service, peer,
        rc == H2_PAL_OK ? H2_BLE_WIFI_CONFIG_STATUS_SUCCESS
                        : H2_BLE_WIFI_CONFIG_STATUS_FAILURE,
        rc == H2_PAL_OK ? H2_BLE_WIFI_CONFIG_REASON_NONE : reason);

    h2_ble_wifi_config_pending_event_t done = {
        .event = rc == H2_PAL_OK ? H2_BLE_WIFI_CONFIG_EVENT_PROVISION_SUCCEEDED
                                 : H2_BLE_WIFI_CONFIG_EVENT_PROVISION_FAILED,
        .conn_handle = conn_handle,
        .status = rc == H2_PAL_OK ? H2_PAL_OK : (int)reason,
    };
    h2_ble_wifi_config_emit(service, &done);
}

static void h2_ble_wifi_config_worker(void *ctx) {
    h2_ble_wifi_config_t *service = ctx;
    for (;;) {
        h2_ble_wifi_config_lock(service);
        while (!service->closing && service->event_count == 0u &&
               !service->reject_pending && !service->credentials_pending &&
               !service->scan_requested) {
            (void)h2_pal_cond_wait(
                service->api.sync, service->cond, service->mutex,
                H2_PAL_SYNC_WAIT_FOREVER);
        }
        if (service->closing) {
            h2_ble_wifi_config_unlock(service);
            return;
        }
        if (service->event_count > 0u) {
            h2_ble_wifi_config_pending_event_t event =
                service->events[service->event_head];
            service->event_head = (service->event_head + 1u) %
                                  H2_BLE_WIFI_CONFIG_EVENT_QUEUE_LEN;
            service->event_count--;
            h2_ble_wifi_config_unlock(service);
            h2_ble_wifi_config_emit(service, &event);
            continue;
        }
        if (service->reject_pending) {
            h2_ble_wifi_config_reason_t reason = service->reject_reason;
            h2_ble_wifi_config_peer_t peer = service->reject_peer;
            service->reject_pending = false;
            h2_ble_wifi_config_unlock(service);
            h2_ble_wifi_config_send_result(
                service, peer, H2_BLE_WIFI_CONFIG_STATUS_FAILURE, reason);
            continue;
        }
        if (service->credentials_pending) {
            h2_ble_wifi_config_credentials_t credentials = service->credentials;
            h2_ble_wifi_config_peer_t peer = {
                .conn_handle = service->conn_handle,
                .generation = service->conn_generation,
            };
            service->credentials_pending = false;
            service->credentials_running = true;
            h2_ble_wifi_config_unlock(service);
            h2_ble_wifi_config_run_provision(service, &credentials, peer);
            h2_ble_wifi_config_lock(service);
            service->credentials_running = false;
            memset(&service->credentials, 0, sizeof(service->credentials));
            h2_ble_wifi_config_unlock(service);
            continue;
        }
        h2_ble_wifi_config_peer_t peer = {
            .conn_handle = service->conn_handle,
            .generation = service->conn_generation,
        };
        service->scan_requested = false;
        service->scan_running = true;
        service->scan_stop_requested = false;
        h2_ble_wifi_config_unlock(service);
        h2_ble_wifi_config_run_scan(service, peer);
        h2_ble_wifi_config_lock(service);
        service->scan_running = false;
        service->scan_stop_requested = false;
        h2_ble_wifi_config_unlock(service);
    }
}

static h2_pal_result_t h2_ble_wifi_config_command_write(
    void *user,
    const h2_pal_ble_gatt_access_t *access,
    const uint8_t *data,
    size_t len) {
    h2_ble_wifi_config_t *service = user;
    if (access == NULL || (len > 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /* Every command fits one ATT value, so a long write is a protocol error. */
    if (access->offset != 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_ble_wifi_config_opcode_t opcode;
    int rc = h2_ble_wifi_config_decode_command(data, len, &opcode);
    h2_ble_wifi_config_lock(service);
    if (rc != H2_PAL_OK) {
        service->stats.protocol_errors++;
        h2_ble_wifi_config_post_event_locked(
            service, H2_BLE_WIFI_CONFIG_EVENT_PROTOCOL_ERROR,
            access->conn_handle, rc);
        h2_ble_wifi_config_unlock(service);
        return (h2_pal_result_t)rc;
    }
    if (service->closing || access->conn_handle != service->conn_handle) {
        h2_ble_wifi_config_unlock(service);
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (opcode == H2_BLE_WIFI_CONFIG_OPCODE_SCAN_STOP) {
        service->scan_requested = false;
        service->scan_stop_requested = true;
        (void)h2_pal_cond_broadcast(service->api.sync, service->cond);
        h2_ble_wifi_config_unlock(service);
        return H2_PAL_OK;
    }
    if (!service->scan_subscribed) {
        service->stats.protocol_errors++;
        h2_ble_wifi_config_post_event_locked(
            service, H2_BLE_WIFI_CONFIG_EVENT_PROTOCOL_ERROR,
            access->conn_handle, H2_PAL_ERR_INVALID_STATE);
        h2_ble_wifi_config_unlock(service);
        return H2_PAL_ERR_INVALID_STATE;
    }
    /* Repeating start while a scan is queued or running is a no-op. */
    if (!service->scan_requested && !service->scan_running) {
        service->scan_requested = true;
        (void)h2_pal_cond_broadcast(service->api.sync, service->cond);
    }
    h2_ble_wifi_config_unlock(service);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_ble_wifi_config_provision_write(
    void *user,
    const h2_pal_ble_gatt_access_t *access,
    const uint8_t *data,
    size_t len) {
    h2_ble_wifi_config_t *service = user;
    if (access == NULL || (len > 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (access->offset != 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_ble_wifi_config_credentials_t credentials;
    int rc = h2_ble_wifi_config_decode_credentials(data, len, &credentials);
    h2_ble_wifi_config_lock(service);
    if (rc != H2_PAL_OK) {
        service->stats.protocol_errors++;
        service->reject_pending = true;
        service->reject_reason = H2_BLE_WIFI_CONFIG_REASON_UNKNOWN;
        service->reject_peer = (h2_ble_wifi_config_peer_t){
            .conn_handle = access->conn_handle,
            .generation = service->conn_generation,
        };
        h2_ble_wifi_config_post_event_locked(
            service, H2_BLE_WIFI_CONFIG_EVENT_PROTOCOL_ERROR,
            access->conn_handle, rc);
        h2_ble_wifi_config_unlock(service);
        return (h2_pal_result_t)rc;
    }
    if (service->closing || access->conn_handle != service->conn_handle) {
        h2_ble_wifi_config_unlock(service);
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (service->credentials_pending || service->credentials_running) {
        h2_ble_wifi_config_unlock(service);
        return H2_PAL_ERR_BUSY;
    }
    service->credentials = credentials;
    service->credentials_pending = true;
    (void)h2_pal_cond_broadcast(service->api.sync, service->cond);
    h2_ble_wifi_config_unlock(service);
    return H2_PAL_OK;
}

/* The caller must hold the mutex. */
static void h2_ble_wifi_config_check_mtu_locked(
    h2_ble_wifi_config_t *service,
    uint16_t conn_handle) {
    if (service->att_mtu != 0u && service->att_mtu < service->config.min_att_mtu) {
        h2_ble_wifi_config_post_event_locked(
            service, H2_BLE_WIFI_CONFIG_EVENT_MTU_TOO_SMALL, conn_handle,
            (int)service->att_mtu);
    }
}

static int h2_ble_wifi_config_system_event(
    void *user,
    const h2_pal_system_event_t *event) {
    h2_ble_wifi_config_t *service = user;
    if (event == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_ble_wifi_config_lock(service);
    switch (event->type) {
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED:
        if (event->payload_size == sizeof(h2_pal_ble_connection_t)) {
            const h2_pal_ble_connection_t *connection = event->payload;
            if (connection->role == H2_PAL_BLE_ROLE_PERIPHERAL &&
                service->conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
                service->conn_handle = connection->conn_handle;
                service->conn_generation++;
                service->att_mtu = connection->mtu;
                service->scan_subscribed = false;
                service->provision_subscribed = false;
                service->stats.att_mtu = connection->mtu;
                h2_ble_wifi_config_post_event_locked(
                    service, H2_BLE_WIFI_CONFIG_EVENT_CONNECTED,
                    connection->conn_handle, H2_PAL_OK);
                h2_ble_wifi_config_check_mtu_locked(
                    service, connection->conn_handle);
            }
        }
        break;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED:
        if (event->payload_size == sizeof(h2_pal_ble_mtu_info_t)) {
            const h2_pal_ble_mtu_info_t *info = event->payload;
            if (info->conn_handle == service->conn_handle) {
                service->att_mtu = info->mtu;
                service->stats.att_mtu = info->mtu;
                h2_ble_wifi_config_check_mtu_locked(service, info->conn_handle);
            }
        }
        break;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED:
        if (event->payload_size == sizeof(h2_pal_ble_subscription_state_t)) {
            const h2_pal_ble_subscription_state_t *state = event->payload;
            if (state->conn_handle == service->conn_handle &&
                state->mode == H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY) {
                if (state->value_handle == service->scan_value_handle) {
                    service->scan_subscribed = state->enabled;
                    if (!state->enabled) {
                        service->scan_stop_requested = true;
                        service->scan_requested = false;
                    }
                } else if (state->value_handle == service->provision_value_handle) {
                    service->provision_subscribed = state->enabled;
                }
            }
        }
        break;
    case H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED:
        if (event->payload_size == sizeof(h2_pal_ble_disconnected_info_t)) {
            const h2_pal_ble_disconnected_info_t *info = event->payload;
            if (info->conn_handle == service->conn_handle) {
                service->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
                service->att_mtu = 0u;
                service->scan_subscribed = false;
                service->provision_subscribed = false;
                service->scan_requested = false;
                service->scan_stop_requested = true;
                service->credentials_pending = false;
                service->reject_pending = false;
                h2_ble_wifi_config_post_event_locked(
                    service, H2_BLE_WIFI_CONFIG_EVENT_DISCONNECTED,
                    info->conn_handle, info->reason);
            }
        }
        break;
    default:
        break;
    }
    h2_ble_wifi_config_unlock(service);
    return H2_PAL_OK;
}

static int h2_ble_wifi_config_subscribe_events(h2_ble_wifi_config_t *service) {
    static const h2_pal_system_event_type_t event_types[] = {
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
    };
    for (size_t i = 0u; i < H2_BLE_WIFI_CONFIG_SUBSCRIPTION_COUNT; ++i) {
        int rc = h2_pal_system_event_subscribe(
            service->api.system_event, event_types[i],
            h2_ble_wifi_config_system_event, service, &service->subscriptions[i]);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static void h2_ble_wifi_config_build_schema(h2_ble_wifi_config_t *service) {
    service->characteristics[0] = (h2_pal_ble_gatt_characteristic_t){
        .uuid = service->config.command_char_uuid,
        .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE,
        .permissions = H2_PAL_BLE_GATT_PERMISSION_WRITE,
        .max_value_len = H2_BLE_WIFI_CONFIG_COMMAND_FRAME_LEN,
        .write = h2_ble_wifi_config_command_write,
        .user = service,
        .out_value_handle = &service->command_value_handle,
    };
    service->characteristics[1] = (h2_pal_ble_gatt_characteristic_t){
        .uuid = service->config.scan_char_uuid,
        .properties = H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
        .permissions = 0u,
        .max_value_len = H2_BLE_WIFI_CONFIG_SCAN_FRAME_MAX_LEN,
        .user = service,
        .out_value_handle = &service->scan_value_handle,
        .out_cccd_handle = &service->scan_cccd_handle,
    };
    service->characteristics[2] = (h2_pal_ble_gatt_characteristic_t){
        .uuid = service->config.provision_char_uuid,
        .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE |
                      H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
        .permissions = H2_PAL_BLE_GATT_PERMISSION_WRITE,
        .max_value_len = H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN,
        .write = h2_ble_wifi_config_provision_write,
        .user = service,
        .out_value_handle = &service->provision_value_handle,
        .out_cccd_handle = &service->provision_cccd_handle,
    };
    service->service = (h2_pal_ble_gatt_service_t){
        .uuid = service->config.service_uuid,
        .primary = true,
        .characteristics = service->characteristics,
        .characteristic_count = H2_BLE_WIFI_CONFIG_CHARACTERISTIC_COUNT,
        .out_service_handle = &service->service_handle,
    };
}

int h2_ble_wifi_config_open(
    const h2_ble_wifi_config_api_t *api,
    const h2_ble_wifi_config_config_t *config,
    h2_ble_wifi_config_t **out_service) {
    if (out_service == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_service = NULL;
    if (!h2_ble_wifi_config_api_valid(api)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_ble_wifi_config_t *service =
        h2_pal_mem_alloc(api->allocator, sizeof(*service));
    if (service == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(service, 0, sizeof(*service));
    service->api = *api;
    if (config != NULL) {
        service->config = *config;
    }
    service->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    service->reject_peer.conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    service->service_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    service->command_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    service->scan_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    service->scan_cccd_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    service->provision_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    service->provision_cccd_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;

    int rc = h2_ble_wifi_config_resolve(service);
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(api->allocator, service);
        return rc;
    }

    h2_pal_mutex_config_t mutex_config = {
        .name = h2_ble_wifi_config_worker_task_name,
        .allocator = api->allocator,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    rc = h2_pal_mutex_create(api->sync, &mutex_config, &service->mutex);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    h2_pal_cond_config_t cond_config = {
        .name = h2_ble_wifi_config_worker_task_name,
        .allocator = api->allocator,
    };
    rc = h2_pal_cond_create(api->sync, &cond_config, &service->cond);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    rc = h2_ble_wifi_config_subscribe_events(service);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    h2_ble_wifi_config_build_schema(service);
    rc = h2_pal_task_start(
        api->task, &service->config.worker_task_options,
        h2_ble_wifi_config_worker, service, &service->worker);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    if (!service->config.gatt_service_registered_by_caller) {
        rc = h2_pal_ble_register_gatt_services(api->ble, &service->service, 1u);
        if (rc != H2_PAL_OK) {
            goto fail;
        }
        service->gatt_registered = true;
    }
    *out_service = service;
    return H2_PAL_OK;

fail:
    if (service->worker != NULL) {
        h2_ble_wifi_config_lock(service);
        service->closing = true;
        (void)h2_pal_cond_broadcast(api->sync, service->cond);
        h2_ble_wifi_config_unlock(service);
        int join_rc = h2_pal_task_join(api->task, service->worker);
        if (join_rc != H2_PAL_OK) {
            return join_rc;
        }
        service->worker = NULL;
    }
    for (size_t i = 0u; i < H2_BLE_WIFI_CONFIG_SUBSCRIPTION_COUNT; ++i) {
        h2_pal_system_event_unsubscribe(api->system_event, service->subscriptions[i]);
    }
    if (service->cond != NULL) {
        (void)h2_pal_cond_destroy(api->sync, service->cond);
    }
    if (service->mutex != NULL) {
        (void)h2_pal_mutex_destroy(api->sync, service->mutex);
    }
    h2_pal_mem_free(api->allocator, service);
    return rc;
}

int h2_ble_wifi_config_close(h2_ble_wifi_config_t *service) {
    if (service == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_ble_wifi_config_lock(service);
    service->closing = true;
    service->scan_stop_requested = true;
    service->scan_requested = false;
    service->credentials_pending = false;
    service->reject_pending = false;
    bool stop_advertising = service->adv_started && !service->adv_paused;
    service->adv_started = false;
    service->adv_paused = false;
    (void)h2_pal_cond_broadcast(service->api.sync, service->cond);
    h2_ble_wifi_config_unlock(service);

    if (service->worker != NULL) {
        int rc = h2_pal_task_join(service->api.task, service->worker);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        service->worker = NULL;
    }
    int result = H2_PAL_OK;
    if (stop_advertising) {
        int rc = h2_pal_ble_stop_advertising(service->api.ble);
        if (rc != H2_PAL_OK) {
            result = rc;
        }
    }
    if (service->gatt_registered) {
        /*
         * The Host borrows the schema, its callback contexts and its handle
         * storage until unregister succeeds. Freeing the service after a
         * failed unregister would leave the Host dispatching writes into
         * released memory, so keep the instance alive and let the caller
         * retry close().
         */
        int rc = h2_pal_ble_unregister_gatt_services(service->api.ble);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        service->gatt_registered = false;
    }
    for (size_t i = 0u; i < H2_BLE_WIFI_CONFIG_SUBSCRIPTION_COUNT; ++i) {
        h2_pal_system_event_unsubscribe(
            service->api.system_event, service->subscriptions[i]);
        service->subscriptions[i] = NULL;
    }
    (void)h2_pal_cond_destroy(service->api.sync, service->cond);
    (void)h2_pal_mutex_destroy(service->api.sync, service->mutex);
    h2_pal_mem_free(service->api.allocator, service);
    return result;
}

int h2_ble_wifi_config_start_advertising(
    h2_ble_wifi_config_t *service,
    const h2_pal_ble_adv_data_t *data,
    const h2_pal_ble_adv_params_t *params) {
    if (service == NULL || params == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_ble_wifi_config_lock(service);
    bool closing = service->closing;
    h2_ble_wifi_config_unlock(service);
    if (closing) {
        return H2_PAL_ERR_CLOSED;
    }
    if (data != NULL) {
        int rc = h2_pal_ble_set_adv_data(service->api.ble, data);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    int rc = h2_pal_ble_start_advertising(service->api.ble, params);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_ble_wifi_config_lock(service);
    service->adv_params = *params;
    service->adv_started = true;
    service->adv_paused = false;
    h2_ble_wifi_config_unlock(service);
    return H2_PAL_OK;
}

int h2_ble_wifi_config_stop_advertising(h2_ble_wifi_config_t *service) {
    if (service == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_ble_wifi_config_lock(service);
    bool stop = service->adv_started && !service->adv_paused;
    service->adv_started = false;
    service->adv_paused = false;
    h2_ble_wifi_config_unlock(service);
    if (!stop) {
        return H2_PAL_OK;
    }
    return h2_pal_ble_stop_advertising(service->api.ble);
}

const h2_pal_ble_gatt_service_t *h2_ble_wifi_config_gatt_service(
    const h2_ble_wifi_config_t *service) {
    return service != NULL ? &service->service : NULL;
}

int h2_ble_wifi_config_get_stats(
    h2_ble_wifi_config_t *service,
    h2_ble_wifi_config_stats_t *out_stats) {
    if (out_stats != NULL) {
        memset(out_stats, 0, sizeof(*out_stats));
    }
    if (service == NULL || out_stats == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_ble_wifi_config_lock(service);
    *out_stats = service->stats;
    h2_ble_wifi_config_unlock(service);
    return H2_PAL_OK;
}
