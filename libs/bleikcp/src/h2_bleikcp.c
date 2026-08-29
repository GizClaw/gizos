#include "h2_bleikcp_internal.h"
#include "h2_bleikcp_task_names.h"

#include <limits.h>
#include <string.h>

static const uint8_t s_service_uuid[] = { 0xe0u, 0xfeu };
static const uint8_t s_tx_uuid[] = { 0xe1u, 0xfeu };
static const uint8_t s_rx_uuid[] = { 0xe2u, 0xfeu };

static bool h2_bleikcp_api_valid(const h2_bleikcp_api_t *api) {
    return api != NULL && api->ble != NULL && api->task != NULL &&
           api->time != NULL && api->sync != NULL &&
           api->system_event != NULL && api->allocator != NULL;
}

static int h2_bleikcp_copy_uuid(
    h2_pal_ble_uuid_t source,
    const uint8_t *default_data,
    size_t default_len,
    uint8_t *storage,
    h2_pal_ble_uuid_t *out) {
    if (source.len == 0u) {
        source.data = default_data;
        source.len = default_len;
    }
    if (source.data == NULL || (source.len != 2u && source.len != 4u && source.len != 16u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(storage, source.data, source.len);
    out->data = storage;
    out->len = source.len;
    return H2_PAL_OK;
}

int h2_bleikcp_resolve_config(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_config_t *config,
    h2_bleikcp_resolved_config_t *out) {
    if (!h2_bleikcp_api_valid(api) || out == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (config != NULL) {
        out->value = *config;
    }
    int rc = h2_bleikcp_copy_uuid(
        out->value.service_uuid, s_service_uuid, sizeof(s_service_uuid),
        out->service_uuid, &out->value.service_uuid);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bleikcp_copy_uuid(
        out->value.tx_char_uuid, s_tx_uuid, sizeof(s_tx_uuid),
        out->tx_uuid, &out->value.tx_char_uuid);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bleikcp_copy_uuid(
        out->value.rx_char_uuid, s_rx_uuid, sizeof(s_rx_uuid),
        out->rx_uuid, &out->value.rx_char_uuid);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    if (out->value.conv == 0u) out->value.conv = H2_BLEIKCP_DEFAULT_CONV;
    if (out->value.max_datagram_len == 0u) out->value.max_datagram_len = H2_BLEIKCP_DEFAULT_MAX_DATAGRAM_LEN;
    if (out->value.send_window == 0u) out->value.send_window = H2_BLEIKCP_DEFAULT_WINDOW;
    if (out->value.recv_window == 0u) out->value.recv_window = H2_BLEIKCP_DEFAULT_WINDOW;
    if (out->value.input_frame_capacity == 0u) out->value.input_frame_capacity = H2_BLEIKCP_DEFAULT_INPUT_FRAME_CAPACITY;
    if (out->value.tx_buffer_size == 0u) out->value.tx_buffer_size = H2_BLEIKCP_DEFAULT_BUFFER_SIZE;
    if (out->value.rx_buffer_size == 0u) out->value.rx_buffer_size = H2_BLEIKCP_DEFAULT_BUFFER_SIZE;
    if (config == NULL || config->nodelay == 0) out->value.nodelay = 1;
    if (out->value.interval_ms == 0) out->value.interval_ms = 10;
    if (out->value.resend == 0) out->value.resend = 2;
    if (config == NULL) out->value.no_congestion_control = 1;
    if (out->value.setup_timeout_ms == 0u) out->value.setup_timeout_ms = 5000u;
    if (out->value.output_retry_count == 0u) out->value.output_retry_count = 5u;
    if (out->value.output_retry_delay_ms == 0u) out->value.output_retry_delay_ms = 5u;
    if (out->value.worker_task_options.name == NULL) out->value.worker_task_options.name = h2_bleikcp_worker_task_name;
    if (out->value.worker_task_options.min_stack_size < 6u * 1024u) out->value.worker_task_options.min_stack_size = 6u * 1024u;
    if (out->value.server_task_options.name == NULL) out->value.server_task_options.name = h2_bleikcp_server_task_name;
    if (out->value.server_task_options.min_stack_size < 6u * 1024u) out->value.server_task_options.min_stack_size = 6u * 1024u;

    if (out->value.max_datagram_len < 50u || out->value.max_datagram_len > H2_PAL_BLE_ATT_MAX_VALUE_LEN ||
        out->value.send_window > UINT16_MAX / 2u || out->value.recv_window > UINT16_MAX / 2u ||
        out->value.input_frame_capacity == 0u ||
        out->value.tx_buffer_size < out->value.max_datagram_len - H2_BLEIKCP_KCP_OVERHEAD ||
        out->value.rx_buffer_size < out->value.max_datagram_len - H2_BLEIKCP_KCP_OVERHEAD ||
        out->value.interval_ms < 1 || out->value.interval_ms > 5000 ||
        out->value.output_retry_count > UINT16_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

bool h2_bleikcp_uuid_equal(const h2_pal_ble_uuid_t *a, const h2_pal_ble_uuid_t *b) {
    return a != NULL && b != NULL && a->data != NULL && b->data != NULL &&
           a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static void *h2_bleikcp_alloc(h2_bleikcp_t *stream, size_t len) {
    return h2_pal_mem_alloc(stream->api.allocator, len);
}

static size_t h2_bleikcp_ring_free(const h2_bleikcp_ring_t *ring) {
    return ring->capacity - ring->len;
}

static void h2_bleikcp_ring_write(h2_bleikcp_ring_t *ring, const uint8_t *data, size_t len) {
    size_t tail = (ring->head + ring->len) % ring->capacity;
    size_t first = ring->capacity - tail;
    if (first > len) first = len;
    memcpy(ring->data + tail, data, first);
    memcpy(ring->data, data + first, len - first);
    ring->len += len;
}

static void h2_bleikcp_ring_read(h2_bleikcp_ring_t *ring, uint8_t *out, size_t len) {
    size_t first = ring->capacity - ring->head;
    if (first > len) first = len;
    memcpy(out, ring->data + ring->head, first);
    memcpy(out + first, ring->data, len - first);
    ring->head = (ring->head + len) % ring->capacity;
    ring->len -= len;
}

static bool h2_bleikcp_retryable(int rc) {
    return rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT ||
           rc == H2_PAL_ERR_UNAVAILABLE;
}

static void h2_bleikcp_emit(
    h2_bleikcp_t *stream,
    h2_bleikcp_event_t event,
    int status) {
    if (stream->config.value.on_event != NULL) {
        stream->config.value.on_event(
            stream->config.value.user, stream, event, stream->conn_handle, status);
    }
}

static int h2_bleikcp_kcp_output(
    const char *buffer,
    int len,
    ikcpcb *kcp,
    void *user) {
    (void)kcp;
    h2_bleikcp_t *stream = user;
    if (len <= 0 || (size_t)len > stream->kcp_mtu) {
        (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
        stream->fatal_status = H2_PAL_ERR_INVALID_ARG;
        (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
        return -1;
    }
    int rc = H2_PAL_ERR_UNSUPPORTED;
    bool emitted = false;
    for (uint32_t attempt = 0u; attempt < stream->config.value.output_retry_count; ++attempt) {
        if (stream->role == H2_BLEIKCP_ROLE_SERVER) {
            rc = h2_pal_ble_notify(
                stream->api.ble, stream->conn_handle, stream->tx_value_handle,
                (const uint8_t *)buffer, (size_t)len);
        } else {
            rc = h2_pal_ble_gatt_write(
                stream->api.ble, stream->conn_handle, stream->rx_value_handle,
                (const uint8_t *)buffer, (size_t)len, false,
                stream->config.value.setup_timeout_ms);
        }
        if (rc == H2_PAL_OK) {
            (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
            stream->stats.tx_frames++;
            (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
            return 0;
        }
        if (!h2_bleikcp_retryable(rc)) break;
        (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
        stream->stats.output_blocked++;
        (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
        if (!emitted) {
            emitted = true;
            h2_bleikcp_emit(stream, H2_BLEIKCP_EVENT_BACKPRESSURE, rc);
        }
        if (attempt + 1u < stream->config.value.output_retry_count) {
            (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
            stream->stats.output_retries++;
            (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
            (void)h2_pal_time_sleep_ms(
                stream->api.time, stream->config.value.output_retry_delay_ms);
        }
    }
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    stream->fatal_status = rc;
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    return -1;
}

static uint32_t h2_bleikcp_now32(h2_bleikcp_t *stream) {
    uint64_t now = 0u;
    if (h2_pal_time_get_monotonic_ms(stream->api.time, &now) != H2_PAL_OK) {
        (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
        stream->fatal_status = H2_PAL_ERR_IO;
        (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    }
    return (uint32_t)now;
}

static void h2_bleikcp_drain_input(h2_bleikcp_t *stream) {
    while (stream->input.count > 0u && stream->fatal_status == H2_PAL_OK) {
        size_t index = stream->input.head;
        size_t len = stream->input.lengths[index];
        const uint8_t *data = stream->input.data + index * stream->input.frame_size;
        if (ikcp_input(stream->kcp, (const char *)data, (long)len) < 0) {
            stream->stats.input_errors++;
            stream->fatal_status = H2_PAL_ERR_IO;
            break;
        }
        stream->stats.rx_frames++;
        stream->input.head = (stream->input.head + 1u) % stream->input.capacity;
        stream->input.count--;
    }
}

static void h2_bleikcp_drain_tx(h2_bleikcp_t *stream) {
    size_t mss = stream->kcp_mtu - H2_BLEIKCP_KCP_OVERHEAD;
    while (stream->tx.len > 0u &&
           ikcp_waitsnd(stream->kcp) < (int)stream->config.value.send_window) {
        size_t len = stream->tx.len < mss ? stream->tx.len : mss;
        h2_bleikcp_ring_read(&stream->tx, stream->scratch, len);
        if (ikcp_send(stream->kcp, (const char *)stream->scratch, (int)len) < 0) {
            stream->fatal_status = H2_PAL_ERR_IO;
            break;
        }
    }
    stream->stats.waitsnd = (uint32_t)ikcp_waitsnd(stream->kcp);
}

static void h2_bleikcp_drain_rx(h2_bleikcp_t *stream) {
    while (stream->fatal_status == H2_PAL_OK) {
        int size = ikcp_peeksize(stream->kcp);
        if (size < 0 || (size_t)size > h2_bleikcp_ring_free(&stream->rx)) return;
        if ((size_t)size > stream->kcp_mtu) {
            stream->fatal_status = H2_PAL_ERR_IO;
            return;
        }
        int rc = ikcp_recv(stream->kcp, (char *)stream->scratch, size);
        if (rc < 0) {
            stream->fatal_status = H2_PAL_ERR_IO;
            return;
        }
        h2_bleikcp_ring_write(&stream->rx, stream->scratch, (size_t)rc);
        if (stream->rx.len > stream->stats.rx_high_water) stream->stats.rx_high_water = stream->rx.len;
    }
}

static void h2_bleikcp_worker(void *ctx) {
    h2_bleikcp_t *stream = ctx;
    bool fatal_emitted = false;
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    bool emit_connected = stream->connected_event_pending;
    bool emit_ready = stream->ready_event_pending;
    stream->connected_event_pending = false;
    stream->ready_event_pending = false;
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    if (emit_connected) {
        h2_bleikcp_emit(stream, H2_BLEIKCP_EVENT_CONNECTED, H2_PAL_OK);
    }
    if (emit_ready) {
        h2_bleikcp_emit(stream, H2_BLEIKCP_EVENT_READY, H2_PAL_OK);
    }
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    while (!stream->closing) {
        h2_bleikcp_drain_input(stream);
        h2_bleikcp_drain_tx(stream);
        (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
        uint32_t now = h2_bleikcp_now32(stream);
        ikcp_update(stream->kcp, now);
        uint32_t next_update = ikcp_check(stream->kcp, now);
        int32_t until_update = (int32_t)(next_update - now);
        uint32_t wait_ms = until_update > 0 ? (uint32_t)until_update : 1u;
        (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
        h2_bleikcp_drain_rx(stream);
        stream->stats.waitsnd = (uint32_t)ikcp_waitsnd(stream->kcp);
        stream->stats.retransmits = stream->kcp->xmit;
        (void)h2_pal_cond_broadcast(stream->api.sync, stream->cond);
        if (stream->fatal_status != H2_PAL_OK) {
            int fatal_status = stream->fatal_status;
            fatal_emitted = true;
            stream->closing = true;
            (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
            h2_bleikcp_emit(stream, H2_BLEIKCP_EVENT_FATAL_ERROR, fatal_status);
            (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
            break;
        }
        (void)h2_pal_cond_wait(
            stream->api.sync, stream->cond, stream->mutex,
            wait_ms);
    }
    bool emit_disconnected = stream->disconnect_event_pending;
    int final_fatal_status = stream->fatal_status;
    stream->disconnect_event_pending = false;
    (void)h2_pal_cond_broadcast(stream->api.sync, stream->cond);
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    if (emit_disconnected) {
        h2_bleikcp_emit(stream, H2_BLEIKCP_EVENT_DISCONNECTED, H2_PAL_ERR_CLOSED);
    }
    if (!fatal_emitted && final_fatal_status != H2_PAL_OK &&
        final_fatal_status != H2_PAL_ERR_CLOSED) {
        h2_bleikcp_emit(
            stream, H2_BLEIKCP_EVENT_FATAL_ERROR, final_fatal_status);
    }
    ikcp_release(stream->kcp);
    stream->kcp = NULL;
}

static int h2_bleikcp_alloc_storage(h2_bleikcp_t *stream) {
    stream->tx.capacity = stream->config.value.tx_buffer_size;
    stream->rx.capacity = stream->config.value.rx_buffer_size;
    stream->input.capacity = stream->config.value.input_frame_capacity;
    stream->input.frame_size = stream->kcp_mtu;
    stream->tx.data = h2_bleikcp_alloc(stream, stream->tx.capacity);
    stream->rx.data = h2_bleikcp_alloc(stream, stream->rx.capacity);
    stream->input.data = h2_bleikcp_alloc(
        stream, stream->input.capacity * stream->input.frame_size);
    stream->input.lengths = h2_bleikcp_alloc(
        stream, stream->input.capacity * sizeof(*stream->input.lengths));
    stream->scratch = h2_bleikcp_alloc(stream, stream->kcp_mtu);
    if (stream->tx.data == NULL || stream->rx.data == NULL ||
        stream->input.data == NULL || stream->input.lengths == NULL ||
        stream->scratch == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    return H2_PAL_OK;
}

int h2_bleikcp_stream_create(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_resolved_config_t *config,
    h2_bleikcp_role_t role,
    uint16_t conn_handle,
    uint16_t att_mtu,
    bool borrowed,
    h2_bleikcp_t **out_stream) {
    if (api == NULL || config == NULL || out_stream == NULL ||
        conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE || att_mtu < H2_BLEIKCP_MIN_ATT_MTU) {
        return att_mtu < H2_BLEIKCP_MIN_ATT_MTU ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_ERR_INVALID_ARG;
    }
    *out_stream = NULL;
    h2_bleikcp_t *stream = h2_pal_mem_alloc(api->allocator, sizeof(*stream));
    if (stream == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(stream, 0, sizeof(*stream));
    stream->api = *api;
    stream->config = *config;
    stream->config.value.service_uuid.data = stream->config.service_uuid;
    stream->config.value.tx_char_uuid.data = stream->config.tx_uuid;
    stream->config.value.rx_char_uuid.data = stream->config.rx_uuid;
    stream->role = role;
    stream->conn_handle = conn_handle;
    stream->att_mtu = att_mtu;
    size_t att_payload = (size_t)att_mtu - H2_PAL_BLE_ATT_HEADER_LEN;
    stream->kcp_mtu = (uint16_t)(att_payload < config->value.max_datagram_len
                                     ? att_payload
                                     : config->value.max_datagram_len);
    stream->borrowed = borrowed;
    stream->fatal_status = H2_PAL_OK;
    stream->connected_event_pending = role == H2_BLEIKCP_ROLE_CLIENT;
    stream->ready_event_pending = true;

    h2_pal_mutex_config_t mutex_config = {
        .name = "bleikcp",
        .allocator = api->allocator,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    int rc = h2_pal_mutex_create(api->sync, &mutex_config, &stream->mutex);
    if (rc != H2_PAL_OK) goto fail;
    h2_pal_cond_config_t cond_config = {
        .name = "bleikcp",
        .allocator = api->allocator,
    };
    rc = h2_pal_cond_create(api->sync, &cond_config, &stream->cond);
    if (rc != H2_PAL_OK) goto fail;
    rc = h2_bleikcp_alloc_storage(stream);
    if (rc != H2_PAL_OK) goto fail;
    stream->kcp = ikcp_create(config->value.conv, stream);
    if (stream->kcp == NULL) {
        rc = H2_PAL_ERR_NO_MEMORY;
        goto fail;
    }
    ikcp_setoutput(stream->kcp, h2_bleikcp_kcp_output);
    if (ikcp_setmtu(stream->kcp, stream->kcp_mtu) != 0 ||
        ikcp_wndsize(stream->kcp, config->value.send_window, config->value.recv_window) != 0 ||
        ikcp_nodelay(stream->kcp, config->value.nodelay, config->value.interval_ms,
            config->value.resend, config->value.no_congestion_control) != 0) {
        rc = H2_PAL_ERR_INVALID_ARG;
        goto fail;
    }
    *out_stream = stream;
    return H2_PAL_OK;

fail:
    (void)h2_bleikcp_stream_destroy(stream);
    return rc;
}

int h2_bleikcp_stream_start(h2_bleikcp_t *stream) {
    if (stream == NULL || stream->worker != NULL) return H2_PAL_ERR_INVALID_ARG;
    int rc = h2_pal_task_start(
        stream->api.task, &stream->config.value.worker_task_options,
        h2_bleikcp_worker, stream, &stream->worker);
    if (rc == H2_PAL_OK) stream->worker_started = true;
    return rc;
}

void h2_bleikcp_stream_mark_closed(h2_bleikcp_t *stream, int status, bool disconnected) {
    if (stream == NULL || stream->mutex == NULL) return;
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    if (!stream->closing) {
        stream->closing = true;
        if (status != H2_PAL_ERR_CLOSED && status != H2_PAL_OK) stream->fatal_status = status;
        if (disconnected) {
            stream->stats.disconnects++;
            stream->disconnect_event_pending = true;
        }
    }
    (void)h2_pal_cond_broadcast(stream->api.sync, stream->cond);
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
}

int h2_bleikcp_stream_join(h2_bleikcp_t *stream) {
    if (stream == NULL) return H2_PAL_OK;
    if (stream->mutex != NULL) h2_bleikcp_stream_mark_closed(stream, H2_PAL_ERR_CLOSED, false);
    if (stream->worker != NULL) {
        int rc = h2_pal_task_join(stream->api.task, stream->worker);
        if (rc != H2_PAL_OK) return rc;
        stream->worker = NULL;
    }
    return H2_PAL_OK;
}

int h2_bleikcp_stream_destroy(h2_bleikcp_t *stream) {
    int rc = h2_bleikcp_stream_join(stream);
    if (rc != H2_PAL_OK || stream == NULL) return rc;
    if (stream->kcp != NULL) ikcp_release(stream->kcp);
    h2_pal_mem_free(stream->api.allocator, stream->scratch);
    h2_pal_mem_free(stream->api.allocator, stream->input.lengths);
    h2_pal_mem_free(stream->api.allocator, stream->input.data);
    h2_pal_mem_free(stream->api.allocator, stream->rx.data);
    h2_pal_mem_free(stream->api.allocator, stream->tx.data);
    if (stream->cond != NULL) (void)h2_pal_cond_destroy(stream->api.sync, stream->cond);
    if (stream->mutex != NULL) (void)h2_pal_mutex_destroy(stream->api.sync, stream->mutex);
    h2_pal_mem_free(stream->api.allocator, stream);
    return H2_PAL_OK;
}

int h2_bleikcp_stream_input(h2_bleikcp_t *stream, const uint8_t *data, size_t len) {
    if (stream == NULL || data == NULL || len == 0u || len > stream->kcp_mtu) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    if (stream->closing) {
        (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
        return H2_PAL_ERR_CLOSED;
    }
    if (stream->input.count == stream->input.capacity) {
        stream->stats.dropped_input++;
        stream->fatal_status = H2_PAL_ERR_FULL;
        stream->closing = true;
        (void)h2_pal_cond_broadcast(stream->api.sync, stream->cond);
        (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
        return H2_PAL_ERR_FULL;
    }
    size_t index = (stream->input.head + stream->input.count) % stream->input.capacity;
    memcpy(stream->input.data + index * stream->input.frame_size, data, len);
    stream->input.lengths[index] = (uint16_t)len;
    stream->input.count++;
    if (stream->input.count > stream->stats.input_high_water) stream->stats.input_high_water = stream->input.count;
    (void)h2_pal_cond_broadcast(stream->api.sync, stream->cond);
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    return H2_PAL_OK;
}

static int h2_bleikcp_wait_locked(
    h2_bleikcp_t *stream,
    uint32_t timeout_ms,
    uint64_t *deadline_ms) {
    if (timeout_ms == 0u) return H2_PAL_ERR_WOULD_BLOCK;
    uint32_t wait_ms = timeout_ms;
    if (timeout_ms != H2_PAL_SYNC_WAIT_FOREVER) {
        uint64_t now_ms = 0u;
        int rc = h2_pal_time_get_monotonic_ms(stream->api.time, &now_ms);
        if (rc != H2_PAL_OK) return rc;
        if (*deadline_ms == 0u) *deadline_ms = h2_pal_time_deadline_ms(now_ms, timeout_ms);
        if (h2_pal_time_deadline_expired(now_ms, *deadline_ms)) return H2_PAL_ERR_TIMEOUT;
        uint64_t remaining = *deadline_ms - now_ms;
        wait_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
        if (wait_ms == 0u) wait_ms = 1u;
    }
    int rc = h2_pal_cond_wait(stream->api.sync, stream->cond, stream->mutex, wait_ms);
    return rc == H2_PAL_OK ? H2_PAL_OK : (rc == H2_PAL_ERR_TIMEOUT ? rc : H2_PAL_ERR_IO);
}

int h2_bleikcp_read(
    h2_bleikcp_t *stream,
    uint8_t *out,
    size_t out_size,
    size_t *out_len,
    uint32_t timeout_ms) {
    if (stream == NULL || out_len == NULL || (out_size > 0u && out == NULL)) return H2_PAL_ERR_INVALID_ARG;
    *out_len = 0u;
    if (out_size == 0u) return H2_PAL_OK;
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    uint64_t deadline_ms = 0u;
    while (stream->rx.len == 0u && !stream->closing) {
        int rc = h2_bleikcp_wait_locked(stream, timeout_ms, &deadline_ms);
        if (rc != H2_PAL_OK) {
            (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
            return rc;
        }
    }
    if (stream->rx.len == 0u) {
        int rc = stream->fatal_status != H2_PAL_OK ? stream->fatal_status : H2_PAL_ERR_CLOSED;
        (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
        return rc;
    }
    size_t len = stream->rx.len < out_size ? stream->rx.len : out_size;
    h2_bleikcp_ring_read(&stream->rx, out, len);
    stream->stats.rx_bytes += len;
    *out_len = len;
    (void)h2_pal_cond_broadcast(stream->api.sync, stream->cond);
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    return H2_PAL_OK;
}

int h2_bleikcp_write(
    h2_bleikcp_t *stream,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    if (stream == NULL || (len > 0u && data == NULL)) return H2_PAL_ERR_INVALID_ARG;
    if (len == 0u) return H2_PAL_OK;
    if (len > stream->tx.capacity) return H2_PAL_ERR_NO_SPACE;
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    uint64_t deadline_ms = 0u;
    while (h2_bleikcp_ring_free(&stream->tx) < len && !stream->closing) {
        int rc = h2_bleikcp_wait_locked(stream, timeout_ms, &deadline_ms);
        if (rc != H2_PAL_OK) {
            (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
            return rc;
        }
    }
    if (stream->closing) {
        int rc = stream->fatal_status != H2_PAL_OK ? stream->fatal_status : H2_PAL_ERR_CLOSED;
        (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
        return rc;
    }
    h2_bleikcp_ring_write(&stream->tx, data, len);
    stream->stats.tx_bytes += len;
    if (stream->tx.len > stream->stats.tx_high_water) stream->stats.tx_high_water = stream->tx.len;
    (void)h2_pal_cond_broadcast(stream->api.sync, stream->cond);
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    return H2_PAL_OK;
}

int h2_bleikcp_flush(h2_bleikcp_t *stream, uint32_t timeout_ms) {
    if (stream == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    uint64_t deadline_ms = 0u;
    while ((stream->tx.len > 0u || stream->stats.waitsnd > 0u) && !stream->closing) {
        int rc = h2_bleikcp_wait_locked(stream, timeout_ms, &deadline_ms);
        if (rc != H2_PAL_OK) {
            (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
            return rc;
        }
    }
    bool pending = stream->tx.len > 0u || stream->stats.waitsnd > 0u;
    int rc = stream->fatal_status != H2_PAL_OK
                 ? stream->fatal_status
                 : (pending ? H2_PAL_ERR_CLOSED : H2_PAL_OK);
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    return rc;
}

int h2_bleikcp_get_stats(h2_bleikcp_t *stream, h2_bleikcp_stats_t *out_stats) {
    if (stream == NULL || out_stats == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)h2_pal_mutex_lock(stream->api.sync, stream->mutex);
    *out_stats = stream->stats;
    out_stats->att_mtu = stream->att_mtu;
    out_stats->kcp_mtu = stream->kcp_mtu;
    (void)h2_pal_mutex_unlock(stream->api.sync, stream->mutex);
    return H2_PAL_OK;
}

int h2_bleikcp_close(h2_bleikcp_t *stream) {
    if (stream == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (stream->borrowed) return H2_PAL_ERR_INVALID_STATE;
    for (size_t i = 0u; i < H2_BLEIKCP_SUBSCRIPTION_COUNT; ++i) {
        h2_pal_system_event_unsubscribe(stream->api.system_event, stream->subscriptions[i]);
        stream->subscriptions[i] = NULL;
    }
    if (stream->role == H2_BLEIKCP_ROLE_CLIENT &&
        stream->tx_cccd_handle != H2_PAL_BLE_INVALID_ATTR_HANDLE && !stream->closing) {
        h2_pal_ble_gatt_subscribe_t subscribe = {
            .value_handle = stream->tx_value_handle,
            .cccd_handle = stream->tx_cccd_handle,
            .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
            .enable = false,
        };
        (void)h2_pal_ble_gatt_subscribe(
            stream->api.ble, stream->conn_handle, &subscribe,
            stream->config.value.setup_timeout_ms);
    }
    return h2_bleikcp_stream_destroy(stream);
}
