#include "h2_peer_internal.h"

#include "ice/h2_peer_ice.h"
#include "media/h2_peer_rtp.h"
#include "providers/h2_peer_portable_backend.h"
#include "sdp/h2_peer_sdp.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef H2_PEER_PRODUCTION_ONLY
#define H2_PEER_PRODUCTION_ONLY 0
#endif

#if H2_PEER_PRODUCTION_ONLY
#define H2_PEER_USES_PRODUCTION_BACKEND(owner) 1
#else
#define H2_PEER_USES_PRODUCTION_BACKEND(owner) ((owner)->production_backend)
#endif

#define H2_PEER_TX_RESERVED ((h2_peer_tx_item_t *)(uintptr_t)1u)

enum {
    H2_PEER_NETWORK_COMMAND_COUNT = 1,
    H2_PEER_NETWORK_EVENT_HIGH_WATER = 12,
    H2_PEER_NETWORK_EVENT_COUNT =
        H2_PEER_STREAM_COUNT + H2_PEER_NETWORK_EVENT_HIGH_WATER + 4,
    H2_PEER_NETWORK_EVENT_BYTES_HIGH_WATER = 16 * 1024,
    H2_PEER_NETWORK_POLL_MS = 2,
    H2_PEER_NETWORK_IDLE_WAIT_MS = 1,
    H2_PEER_NETWORK_UDP_BURST_MAX = 16,
    H2_PEER_NETWORK_STACK_SIZE = 32 * 1024,
    H2_PEER_PERF_REPORT_INTERVAL_US = 5 * 1000 * 1000,
    H2_PEER_RECEIVE_SLOT_COUNT = H2_PEER_OUTPUT_SLOT_COUNT,
};

static uint64_t h2_peer_round_average_us(const h2_peer_round_stats_t *stats) {
    return stats->count == 0u ? 0u : stats->total_us / stats->count;
}

static void h2_peer_network_record_round(h2_pal_webrtc_peer_t *peer,
                                         h2_peer_round_stats_t *stats,
                                         uint64_t started_us) {
    uint64_t now_us = 0u;
    if (h2_pal_time_get_monotonic_us(peer->owner->config.time, &now_us) !=
            H2_PAL_OK ||
        now_us < started_us) {
        return;
    }
    const uint64_t elapsed_us = now_us - started_us;
    ++stats->count;
    stats->total_us += elapsed_us;
    if (elapsed_us > stats->max_us) {
        stats->max_us = elapsed_us;
    }
    if (peer->perf_window_started_us == 0u) {
        peer->perf_window_started_us = now_us;
        return;
    }
    if (now_us - peer->perf_window_started_us <
        H2_PEER_PERF_REPORT_INTERVAL_US) {
        return;
    }
    if (peer->owner->config.log->vtable->write != NULL) {
        char message[H2_PAL_LOG_MESSAGE_MAX];
        (void)snprintf(
            message, sizeof(message),
            "round_us command=%" PRIu64 "/%" PRIu64 "/%" PRIu64 " send=%" PRIu64
            "/%" PRIu64 "/%" PRIu64 " transport=%" PRIu64 "/%" PRIu64
            "/%" PRIu64 " idle=%" PRIu64 "/%" PRIu64 "/%" PRIu64,
            peer->perf_command.count,
            h2_peer_round_average_us(&peer->perf_command),
            peer->perf_command.max_us, peer->perf_send.count,
            h2_peer_round_average_us(&peer->perf_send), peer->perf_send.max_us,
            peer->perf_transport.count,
            h2_peer_round_average_us(&peer->perf_transport),
            peer->perf_transport.max_us, peer->perf_idle.count,
            h2_peer_round_average_us(&peer->perf_idle), peer->perf_idle.max_us);
        (void)h2_pal_log_write(peer->owner->config.log, H2_PAL_LOG_INFO,
                               "h2peer/perf", message);
    }
    peer->perf_command = (h2_peer_round_stats_t){0};
    peer->perf_send = (h2_peer_round_stats_t){0};
    peer->perf_transport = (h2_peer_round_stats_t){0};
    peer->perf_idle = (h2_peer_round_stats_t){0};
    peer->perf_window_started_us = now_us;
}

typedef enum h2_peer_network_command_type {
    H2_PEER_NETWORK_ADD_ICE_SERVER = 1,
    H2_PEER_NETWORK_START_OFFER,
    H2_PEER_NETWORK_SET_REMOTE_SDP,
    H2_PEER_NETWORK_CREATE_DATA_CHANNEL,
    H2_PEER_NETWORK_SET_MEDIA_TRACK,
    H2_PEER_NETWORK_CHANNEL_CLOSE,
    H2_PEER_NETWORK_PEER_CLOSE,
} h2_peer_network_command_type_t;

typedef struct h2_peer_network_command {
    h2_peer_network_command_type_t type;
    union {
        const h2_pal_webrtc_ice_server_t *ice_server;
        struct {
            h2_pal_webrtc_sdp_type_t type;
            h2_pal_webrtc_str_t sdp;
        } remote_sdp;
        const h2_pal_webrtc_channel_config_t *channel_config;
        h2_pal_webrtc_track_t *media_track;
        h2_pal_webrtc_channel_t *channel;
    } value;
} h2_peer_network_command_t;

typedef struct h2_peer_network_response {
    h2_pal_result_t result;
    h2_pal_webrtc_channel_t *channel;
} h2_peer_network_response_t;

typedef enum h2_peer_network_event_type {
    H2_PEER_NETWORK_EVENT_PEER_STATE = 1,
    H2_PEER_NETWORK_EVENT_LOCAL_SDP,
    H2_PEER_NETWORK_EVENT_CHANNEL_STATE,
    H2_PEER_NETWORK_EVENT_CHANNEL_MESSAGE,
    H2_PEER_NETWORK_EVENT_OPUS_FRAME,
    H2_PEER_NETWORK_EVENT_RECEIVE_READY,
    H2_PEER_NETWORK_EVENT_SEND_READY,
} h2_peer_network_event_type_t;

typedef struct h2_peer_network_event {
    h2_peer_network_event_type_t type;
    h2_pal_webrtc_peer_t *peer;
    h2_pal_webrtc_channel_t *channel;
    h2_pal_webrtc_peer_state_t peer_state;
    h2_pal_webrtc_channel_state_t channel_state;
    h2_pal_webrtc_sdp_type_t sdp_type;
    uint8_t *data;
    size_t data_len;
    int is_text;
} h2_peer_network_event_t;

static h2_pal_result_t
h2_peer_queue_network_event(h2_pal_webrtc_peer_t *peer,
                            h2_peer_network_event_t *event, const uint8_t *data,
                            size_t data_len);

static void *h2_peer_alloc(h2_peer_t *owner, size_t len) {
    void *ptr = h2_pal_mem_alloc(owner->config.mem, len);
    if (ptr != NULL) {
        memset(ptr, 0, len);
    }
    return ptr;
}

static void h2_peer_free(h2_peer_t *owner, void *ptr) {
    h2_pal_mem_free(owner->config.mem, ptr);
}

static const h2_pal_mem_api_t *h2_peer_control_mem(const h2_peer_t *owner) {
    return owner->config.control_mem != NULL ? owner->config.control_mem
                                             : owner->config.mem;
}

static void *h2_peer_control_alloc(h2_peer_t *owner, size_t len) {
    void *ptr = h2_pal_mem_alloc(h2_peer_control_mem(owner), len);
    if (ptr != NULL) {
        memset(ptr, 0, len);
    }
    return ptr;
}

static void h2_peer_control_free(h2_peer_t *owner, void *ptr) {
    h2_pal_mem_free(h2_peer_control_mem(owner), ptr);
}

static void h2_peer_free_tx_item(h2_peer_t *owner, h2_peer_tx_item_t *item) {
    if (item == NULL || item == H2_PEER_TX_RESERVED) {
        return;
    }
    h2_peer_free(owner, item->data);
    h2_peer_free(owner, item);
}

static h2_peer_tx_item_t *h2_peer_prepare_tx_item(h2_peer_t *owner,
                                                  h2_peer_tx_item_t **storage,
                                                  const uint8_t *data,
                                                  size_t len, int is_text) {
    h2_peer_tx_item_t *item = *storage;
    if (item == NULL || item == H2_PEER_TX_RESERVED) {
        item = h2_peer_alloc(owner, sizeof(*item));
        if (item == NULL) {
            return NULL;
        }
        *storage = item;
    }
    if (len > item->capacity) {
        uint8_t *replacement = h2_peer_alloc(owner, len);
        if (replacement == NULL) {
            return NULL;
        }
        h2_peer_free(owner, item->data);
        item->data = replacement;
        item->capacity = len;
    }
    if (len != 0u) {
        memcpy(item->data, data, len);
    }
    item->len = len;
    item->is_text = is_text != 0;
    return item;
}

static void h2_peer_network_notify_send_ready(h2_pal_webrtc_peer_t *peer) {
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &peer->network_send_wakeup_queued, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        return;
    }
    h2_peer_network_event_t event = {
        .type = H2_PEER_NETWORK_EVENT_SEND_READY,
    };
    if (h2_peer_queue_network_event(peer, &event, NULL, 0u) != H2_PAL_OK) {
        atomic_store_explicit(&peer->network_send_wakeup_queued, 0,
                              memory_order_release);
    }
}

static h2_pal_result_t h2_peer_receive_gate_create(h2_pal_webrtc_peer_t *peer,
                                                   const char *name,
                                                   h2_pal_queue_t **out_gate) {
    const h2_pal_queue_config_t config = {
        .name = name,
        .item_size = sizeof(uint8_t),
        .item_count = 1u,
        .allocator = peer->owner->config.mem,
    };
    return (h2_pal_result_t)h2_pal_queue_create(peer->owner->config.queue,
                                                &config, out_gate);
}

static h2_pal_result_t
h2_peer_receive_submit(h2_pal_webrtc_peer_t *peer, h2_peer_tx_item_t **storage,
                       atomic_uchar *state, atomic_uint *count,
                       size_t *write_cursor, h2_pal_queue_t *gate,
                       const uint8_t *data, size_t len, int is_text) {
    size_t slot = H2_PEER_RECEIVE_SLOT_COUNT;
    for (size_t offset = 0u; offset < H2_PEER_RECEIVE_SLOT_COUNT; ++offset) {
        const size_t candidate =
            (*write_cursor + offset) % H2_PEER_RECEIVE_SLOT_COUNT;
        unsigned char expected = 0u;
        if (atomic_compare_exchange_strong_explicit(
                &state[candidate], &expected, 1u, memory_order_acq_rel,
                memory_order_relaxed)) {
            slot = candidate;
            break;
        }
    }
    if (slot == H2_PEER_RECEIVE_SLOT_COUNT) {
        return H2_PAL_ERR_FULL;
    }
    h2_peer_tx_item_t *item = h2_peer_prepare_tx_item(
        peer->owner, &storage[slot], data, len, is_text);
    if (item == NULL) {
        atomic_store_explicit(&state[slot], 0u, memory_order_release);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *write_cursor = (slot + 1u) % H2_PEER_RECEIVE_SLOT_COUNT;
    atomic_fetch_add_explicit(&peer->network_receive_count, 1u,
                              memory_order_relaxed);
    const unsigned int previous =
        atomic_fetch_add_explicit(count, 1u, memory_order_acq_rel);
    if (previous + 1u == H2_PEER_RECEIVE_SLOT_COUNT) {
        atomic_fetch_add_explicit(&peer->network_receive_full, 1u,
                                  memory_order_release);
    }
    atomic_store_explicit(&state[slot], 2u, memory_order_release);
    const uint8_t token = 1u;
    (void)h2_pal_queue_send_latest(peer->owner->config.queue, gate, &token);
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &peer->network_receive_wakeup_queued, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        h2_peer_network_event_t event = {
            .type = H2_PEER_NETWORK_EVENT_RECEIVE_READY,
        };
        h2_pal_result_t wake_result =
            h2_peer_queue_network_event(peer, &event, NULL, 0u);
        if (wake_result != H2_PAL_OK) {
            atomic_store_explicit(&peer->network_receive_wakeup_queued, 0,
                                  memory_order_release);
            return wake_result;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_receive_take(h2_pal_webrtc_peer_t *peer, h2_peer_tx_item_t **storage,
                     atomic_uchar *state, atomic_uint *count,
                     size_t *read_cursor, h2_pal_queue_t *gate, uint8_t *data,
                     size_t capacity, size_t *out_len, int *out_is_text,
                     uint32_t timeout_ms) {
    uint8_t token = 0u;
    h2_pal_result_t result = (h2_pal_result_t)h2_pal_queue_recv(
        peer->owner->config.queue, gate, &token, timeout_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    size_t slot = H2_PEER_RECEIVE_SLOT_COUNT;
    for (size_t offset = 0u; offset < H2_PEER_RECEIVE_SLOT_COUNT; ++offset) {
        const size_t candidate =
            (*read_cursor + offset) % H2_PEER_RECEIVE_SLOT_COUNT;
        unsigned char expected = 2u;
        if (atomic_compare_exchange_strong_explicit(
                &state[candidate], &expected, 1u, memory_order_acq_rel,
                memory_order_relaxed)) {
            slot = candidate;
            break;
        }
    }
    if (slot == H2_PEER_RECEIVE_SLOT_COUNT) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    h2_peer_tx_item_t *item = storage[slot];
    *out_len = item->len;
    if (capacity < item->len) {
        atomic_store_explicit(&state[slot], 2u, memory_order_release);
        const uint8_t retry_token = 1u;
        (void)h2_pal_queue_send_latest(peer->owner->config.queue, gate,
                                       &retry_token);
        return H2_PAL_ERR_NO_SPACE;
    }
    if (item->len != 0u) {
        memcpy(data, item->data, item->len);
    }
    if (out_is_text != NULL) {
        *out_is_text = item->is_text;
    }
    *read_cursor = (slot + 1u) % H2_PEER_RECEIVE_SLOT_COUNT;
    atomic_store_explicit(&state[slot], 0u, memory_order_release);
    atomic_fetch_sub_explicit(&peer->network_receive_count, 1u,
                              memory_order_relaxed);
    const unsigned int previous =
        atomic_fetch_sub_explicit(count, 1u, memory_order_acq_rel);
    if (previous == H2_PEER_RECEIVE_SLOT_COUNT) {
        atomic_fetch_sub_explicit(&peer->network_receive_full, 1u,
                                  memory_order_release);
    }
    for (size_t i = 0u; i < H2_PEER_RECEIVE_SLOT_COUNT; ++i) {
        if (atomic_load_explicit(&state[i], memory_order_acquire) == 2u) {
            const uint8_t next_token = 1u;
            (void)h2_pal_queue_send_latest(peer->owner->config.queue, gate,
                                           &next_token);
            break;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_channel_ready_slot_allocate(h2_pal_webrtc_channel_t *channel) {
    h2_pal_webrtc_peer_t *peer = channel->owner;
    for (uint8_t slot = 0u; slot < H2_PEER_READY_CHANNEL_COUNT; ++slot) {
        bool used = false;
        for (h2_pal_webrtc_channel_t *existing = peer->channels;
             existing != NULL; existing = existing->next) {
            if (existing->ready_slot == slot) {
                used = true;
                break;
            }
        }
        if (!used) {
            channel->ready_slot = slot;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NO_SPACE;
}

static void h2_peer_channel_ready_set(h2_pal_webrtc_channel_t *channel) {
    if (channel->ready_slot >= H2_PEER_READY_CHANNEL_COUNT) {
        return;
    }
    atomic_fetch_or_explicit(&channel->owner->channel_ready,
                             UINT32_C(1) << channel->ready_slot,
                             memory_order_release);
}

static void h2_peer_channel_ready_clear(h2_pal_webrtc_channel_t *channel) {
    if (channel->ready_slot >= H2_PEER_READY_CHANNEL_COUNT) {
        return;
    }
    const uint32_t bit = UINT32_C(1) << channel->ready_slot;
    atomic_fetch_and_explicit(&channel->owner->channel_ready, ~bit,
                              memory_order_acq_rel);
    channel->ready_slot = UINT8_MAX;
}

static char *h2_peer_copy_string(h2_peer_t *owner, h2_pal_webrtc_str_t value) {
    if (value.len == 0u) {
        return NULL;
    }
    if (value.data == NULL || value.len == SIZE_MAX) {
        return NULL;
    }
    char *copy = (char *)h2_peer_alloc(owner, value.len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value.data, value.len);
    copy[value.len] = '\0';
    return copy;
}

static int h2_peer_config_valid(const h2_peer_config_t *config) {
    return config != NULL && config->mem != NULL &&
           config->mem->vtable != NULL && config->mem->vtable->alloc != NULL &&
           config->mem->vtable->free != NULL &&
           (config->control_mem == NULL ||
            (config->control_mem->vtable != NULL &&
             config->control_mem->vtable->alloc != NULL &&
             config->control_mem->vtable->free != NULL)) &&
           config->log != NULL && config->log->vtable != NULL &&
           config->log->vtable->write != NULL && config->net != NULL &&
           config->net->vtable != NULL && config->queue != NULL &&
           config->queue->vtable != NULL && config->sync != NULL &&
           config->sync->vtable != NULL && config->task != NULL &&
           config->task->vtable != NULL && config->time != NULL &&
           config->time->vtable != NULL &&
           config->time->vtable->get_monotonic_ms != NULL &&
           config->time->vtable->get_monotonic_us != NULL &&
           config->crypto != NULL && config->crypto->vtable != NULL &&
           config->crypto->vtable->random != NULL && config->dtls != NULL &&
           config->dtls->vtable != NULL && config->sctp != NULL &&
           config->sctp->vtable != NULL;
}

static int h2_peer_production_config_supported(const h2_peer_config_t *config) {
    const h2_pal_net_vtable_t *net = config->net->vtable;
    return net->resolve_addr != NULL && net->get_host_addr != NULL &&
           net->udp_open != NULL && net->udp_sendto != NULL &&
           net->udp_recvfrom != NULL && net->tcp_open != NULL &&
           net->tcp_open_bound != NULL && net->tcp_connect != NULL &&
           net->tcp_send_timeout != NULL && net->tcp_recv != NULL &&
           net->close != NULL && config->queue->vtable->create != NULL &&
           config->queue->vtable->destroy != NULL &&
           config->queue->vtable->send != NULL &&
           config->queue->vtable->send_latest != NULL &&
           config->queue->vtable->recv != NULL &&
           config->sync->vtable->create_mutex != NULL &&
           config->sync->vtable->destroy_mutex != NULL &&
           config->sync->vtable->lock_mutex != NULL &&
           config->sync->vtable->try_lock_mutex != NULL &&
           config->sync->vtable->unlock_mutex != NULL &&
           config->sync->vtable->create_cond != NULL &&
           config->sync->vtable->destroy_cond != NULL &&
           config->sync->vtable->wait_cond != NULL &&
           config->sync->vtable->broadcast_cond != NULL &&
           config->task->vtable->start != NULL &&
           config->task->vtable->join != NULL &&
           config->time->vtable->sleep_ms != NULL &&
           config->dtls->vtable->session_create != NULL &&
           config->dtls->vtable->session_get_local_fingerprint != NULL &&
           config->dtls->vtable->session_set_remote_fingerprint != NULL &&
           config->dtls->vtable->session_handshake != NULL &&
           config->dtls->vtable->session_next_deadline_ms != NULL &&
           config->dtls->vtable->session_flush != NULL &&
           config->dtls->vtable->session_get_srtp_profile != NULL &&
           config->dtls->vtable->session_export_srtp_keying_material != NULL &&
           config->dtls->vtable->session_write != NULL &&
           config->dtls->vtable->session_consume_datagram != NULL &&
           config->dtls->vtable->session_close != NULL &&
           config->dtls->vtable->session_destroy != NULL &&
           config->sctp->vtable->association_create != NULL &&
           config->sctp->vtable->association_start != NULL &&
           config->sctp->vtable->association_input_packet != NULL &&
           config->sctp->vtable->association_service != NULL &&
           config->sctp->vtable->association_send_message != NULL &&
           config->sctp->vtable->association_is_writable != NULL &&
           config->sctp->vtable->association_reset_stream != NULL &&
           config->sctp->vtable->association_shutdown != NULL &&
           config->sctp->vtable->association_abort != NULL &&
           config->sctp->vtable->association_close != NULL;
}

#if !H2_PEER_PRODUCTION_ONLY
static int h2_peer_providers_valid(const h2_peer_provider_bundle_t *providers) {
    return providers != NULL && providers->ice.vtable != NULL &&
           providers->ice.vtable->open != NULL &&
           providers->ice.vtable->poll != NULL &&
           providers->ice.vtable->close != NULL &&
           providers->dtls.vtable != NULL &&
           providers->dtls.vtable->open != NULL &&
           providers->dtls.vtable->get_local_fingerprint != NULL &&
           providers->dtls.vtable->set_remote_fingerprint != NULL &&
           providers->dtls.vtable->poll != NULL &&
           providers->dtls.vtable->close != NULL &&
           providers->srtp.vtable != NULL &&
           providers->srtp.vtable->open != NULL &&
           providers->srtp.vtable->send_rtp != NULL &&
           providers->srtp.vtable->receive_rtp != NULL &&
           providers->srtp.vtable->close != NULL &&
           providers->sctp.vtable != NULL &&
           providers->sctp.vtable->open != NULL &&
           providers->sctp.vtable->poll != NULL &&
           providers->sctp.vtable->channel_open != NULL &&
           providers->sctp.vtable->send != NULL &&
           providers->sctp.vtable->channel_close != NULL &&
           providers->sctp.vtable->close != NULL;
}
#endif

static void h2_peer_free_channel(h2_pal_webrtc_peer_t *peer,
                                 h2_pal_webrtc_channel_t *channel);

static int h2_peer_network_enabled(const h2_pal_webrtc_peer_t *peer) {
    return H2_PEER_USES_PRODUCTION_BACKEND(peer->owner) &&
           peer->network_events != NULL;
}

static int h2_peer_channel_pull_enabled(const h2_pal_webrtc_peer_t *peer) {
    return (peer->receive_flags & H2_PAL_WEBRTC_RECEIVE_CHANNEL_PULL) != 0u;
}

static int h2_peer_opus_pull_enabled(const h2_pal_webrtc_peer_t *peer) {
    return (peer->receive_flags & H2_PAL_WEBRTC_RECEIVE_OPUS_PULL) != 0u;
}

static h2_pal_result_t
h2_peer_queue_network_event(h2_pal_webrtc_peer_t *peer,
                            h2_peer_network_event_t *event, const uint8_t *data,
                            size_t data_len) {
    if (data_len != 0u && data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_peer_network_event_t *queued_event =
        h2_peer_alloc(peer->owner, sizeof(*queued_event) + data_len);
    if (queued_event == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    *queued_event = *event;
    queued_event->peer = peer;
    if (data_len != 0u) {
        queued_event->data = (uint8_t *)(queued_event + 1);
        memcpy(queued_event->data, data, data_len);
        queued_event->data_len = data_len;
    }
    if (queued_event->channel != NULL) {
        atomic_fetch_add_explicit(&queued_event->channel->callback_refs, 1u,
                                  memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&peer->network_event_count, 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&peer->network_event_bytes, data_len,
                              memory_order_relaxed);
    h2_pal_result_t result = (h2_pal_result_t)h2_pal_queue_send(
        peer->owner->config.queue, peer->network_events, &queued_event,
        H2_PAL_QUEUE_NO_WAIT);
    if (result == H2_PAL_OK) {
        return H2_PAL_OK;
    }
    atomic_fetch_sub_explicit(&peer->network_event_bytes, data_len,
                              memory_order_relaxed);
    atomic_fetch_sub_explicit(&peer->network_event_count, 1u,
                              memory_order_relaxed);
    if (queued_event->channel != NULL) {
        atomic_fetch_sub_explicit(&queued_event->channel->callback_refs, 1u,
                                  memory_order_relaxed);
    }
    h2_peer_free(peer->owner, queued_event);
    return result;
}

static void h2_peer_record_network_event_error(h2_pal_webrtc_peer_t *peer,
                                               h2_pal_result_t result) {
    if (result == H2_PAL_OK) {
        return;
    }
    int expected = H2_PAL_OK;
    (void)atomic_compare_exchange_strong_explicit(
        &peer->network_transport_result, &expected, result,
        memory_order_release, memory_order_relaxed);
}

void h2_peer_webrtc_emit_peer_state(h2_pal_webrtc_peer_t *peer,
                                    h2_pal_webrtc_peer_state_t state) {
    if (peer->state == state) {
        return;
    }
    peer->state = state;
    if (h2_peer_network_enabled(peer)) {
        h2_peer_network_event_t event = {
            .type = H2_PEER_NETWORK_EVENT_PEER_STATE,
            .peer_state = state,
        };
        h2_peer_record_network_event_error(
            peer, h2_peer_queue_network_event(peer, &event, NULL, 0u));
        return;
    }
    if (peer->callbacks.on_peer_state != NULL) {
        peer->callbacks.on_peer_state(peer->callbacks.user, peer, state);
    }
}

void h2_peer_webrtc_emit_channel_state(h2_pal_webrtc_channel_t *channel,
                                       h2_pal_webrtc_channel_state_t state) {
    h2_pal_webrtc_peer_t *peer = channel->owner;
    if (h2_peer_network_enabled(peer)) {
        h2_peer_network_event_t event = {
            .type = H2_PEER_NETWORK_EVENT_CHANNEL_STATE,
            .channel = channel,
            .channel_state = state,
        };
        h2_peer_record_network_event_error(
            peer, h2_peer_queue_network_event(peer, &event, NULL, 0u));
        return;
    }
    if (channel->owner->callbacks.on_channel_state != NULL) {
        channel->owner->callbacks.on_channel_state(
            channel->owner->callbacks.user, channel->owner, channel,
            &channel->info, state);
    }
}

void h2_peer_webrtc_emit_local_sdp(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_sdp_type_t type,
                                   h2_pal_webrtc_str_t sdp) {
    if (h2_peer_network_enabled(peer)) {
        h2_peer_network_event_t event = {
            .type = H2_PEER_NETWORK_EVENT_LOCAL_SDP,
            .sdp_type = type,
        };
        h2_peer_record_network_event_error(
            peer, h2_peer_queue_network_event(
                      peer, &event, (const uint8_t *)sdp.data, sdp.len));
        return;
    }
    if (peer->callbacks.on_local_sdp != NULL) {
        peer->callbacks.on_local_sdp(peer->callbacks.user, peer, type, sdp);
    }
}

h2_pal_result_t h2_peer_webrtc_emit_channel_message(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel,
    const uint8_t *data, size_t len, int is_text) {
    if (h2_peer_network_enabled(peer)) {
        if (h2_peer_channel_pull_enabled(peer)) {
            h2_pal_result_t result = h2_peer_receive_submit(
                peer, channel->rx_storage, channel->rx_state,
                &channel->rx_count, &channel->rx_write_cursor, channel->rx_gate,
                data, len, is_text);
            return result == H2_PAL_ERR_FULL ? H2_PAL_ERR_WOULD_BLOCK : result;
        }
        if (peer->callbacks.on_channel_message == NULL) {
            return H2_PAL_OK;
        }
        h2_peer_network_event_t event = {
            .type = H2_PEER_NETWORK_EVENT_CHANNEL_MESSAGE,
            .channel = channel,
            .is_text = is_text != 0,
        };
        h2_pal_result_t result =
            h2_peer_queue_network_event(peer, &event, data, len);
        return result == H2_PAL_ERR_FULL ? H2_PAL_ERR_WOULD_BLOCK : result;
    }
    if (peer->callbacks.on_channel_message != NULL) {
        peer->callbacks.on_channel_message(peer->callbacks.user, peer, channel,
                                           &channel->info, data, len,
                                           is_text != 0);
    }
    return H2_PAL_OK;
}

void h2_peer_webrtc_emit_opus_frame(h2_pal_webrtc_peer_t *peer,
                                    const uint8_t *opus, size_t opus_len) {
    if (h2_peer_network_enabled(peer)) {
        if (peer->media_track != NULL &&
            peer->media_track->config.write != NULL) {
            h2_peer_record_network_event_error(
                peer, peer->media_track->config.write(
                          peer->media_track->config.user, opus, opus_len));
            return;
        }
        if (h2_peer_opus_pull_enabled(peer)) {
            h2_peer_record_network_event_error(
                peer, h2_peer_receive_submit(
                          peer, peer->opus_rx_storage, peer->opus_rx_state,
                          &peer->opus_rx_count, &peer->opus_rx_write_cursor,
                          peer->opus_rx_gate, opus, opus_len, 0));
            return;
        }
        if (peer->callbacks.on_opus_frame == NULL) {
            return;
        }
        h2_peer_network_event_t event = {
            .type = H2_PEER_NETWORK_EVENT_OPUS_FRAME,
        };
        h2_peer_record_network_event_error(
            peer, h2_peer_queue_network_event(peer, &event, opus, opus_len));
        return;
    }
    if (peer->media_track != NULL && peer->media_track->config.write != NULL) {
        const h2_pal_result_t write_result = peer->media_track->config.write(
            peer->media_track->config.user, opus, opus_len);
        if (write_result != H2_PAL_OK)
            h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_FAILED);
    } else if (peer->callbacks.on_opus_frame != NULL) {
        peer->callbacks.on_opus_frame(peer->callbacks.user, peer, opus,
                                      opus_len);
    }
}

static h2_pal_webrtc_channel_t *h2_peer_find_channel(h2_pal_webrtc_peer_t *peer,
                                                     uint16_t stream_id) {
    for (h2_pal_webrtc_channel_t *channel = peer->channels; channel != NULL;
         channel = channel->next) {
        if (channel->info.stream_id == stream_id) {
            return channel;
        }
    }
    return NULL;
}

static int h2_peer_channel_is_current(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_webrtc_channel_t *channel,
                                      uint16_t stream_id, uint32_t generation) {
    h2_pal_webrtc_channel_t *current = h2_peer_find_channel(peer, stream_id);
    return current == channel && current->generation == generation;
}

static void h2_peer_free_channel(h2_pal_webrtc_peer_t *peer,
                                 h2_pal_webrtc_channel_t *channel);

static void h2_peer_close_providers(h2_pal_webrtc_peer_t *peer);
static h2_pal_result_t
h2_peer_open_pending_channels(h2_pal_webrtc_peer_t *peer);

static int h2_peer_has_active_stream_reset(const h2_pal_webrtc_peer_t *peer) {
    for (size_t i = 0u; i < H2_PEER_STREAM_COUNT; ++i) {
        if (peer->stream_resets[i].active) {
            return 1;
        }
    }
    return 0;
}

static int h2_peer_stream_slot(const h2_pal_webrtc_peer_t *peer,
                               uint16_t stream_id, size_t *out_slot) {
    if (stream_id < peer->local_stream_first) {
        return 0;
    }
    uint16_t offset = (uint16_t)(stream_id - peer->local_stream_first);
    size_t slot = (size_t)(offset / 2u);
    if ((offset & 1u) != 0u || slot >= H2_PEER_LOCAL_STREAM_COUNT) {
        return 0;
    }
    *out_slot = slot;
    return 1;
}

static uint16_t h2_peer_stream_id_for_slot(const h2_pal_webrtc_peer_t *peer,
                                           size_t slot) {
    return (uint16_t)(peer->local_stream_first + (uint16_t)(slot * 2u));
}

static void h2_peer_unlink_channel(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_channel_t *channel) {
    h2_pal_webrtc_channel_t **cursor = &peer->channels;
    while (*cursor != NULL && *cursor != channel) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == channel) {
        *cursor = channel->next;
    }
    channel->next = NULL;
}

static void h2_peer_terminal_channel(h2_pal_webrtc_channel_t *channel,
                                     h2_pal_webrtc_channel_state_t state) {
    if (channel == NULL || channel->terminal) {
        return;
    }
    h2_pal_webrtc_peer_t *peer = channel->owner;
    channel->terminal = 1;
    channel->open = 0;
    h2_peer_unlink_channel(peer, channel);
    h2_peer_webrtc_emit_channel_state(channel, state);
    if (h2_peer_network_enabled(peer)) {
        unsigned int previous = atomic_fetch_or_explicit(
            &channel->callback_refs, H2_PEER_CHANNEL_FREE_PENDING,
            memory_order_acq_rel);
        if (previous == 0u) {
            h2_peer_free_channel(peer, channel);
        }
    } else {
        h2_peer_free_channel(peer, channel);
    }
}

static void h2_peer_terminal_all_channels(h2_pal_webrtc_peer_t *peer,
                                          h2_pal_webrtc_channel_state_t state) {
    while (peer->channels != NULL) {
        h2_peer_terminal_channel(peer->channels, state);
    }
}

static h2_pal_result_t h2_peer_submit_stream_reset(h2_pal_webrtc_peer_t *peer,
                                                   uint16_t stream_id) {
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        return h2_peer_portable_reset_stream(peer, stream_id);
    }
    return peer->owner->providers.sctp.vtable->channel_close(
        peer->owner->providers.sctp.user, peer->sctp_session, stream_id);
}

static h2_pal_result_t h2_peer_forget_stream(h2_pal_webrtc_peer_t *peer,
                                             uint16_t stream_id) {
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        return h2_peer_portable_forget_stream(peer, stream_id);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_peer_fail_stream_resets(h2_pal_webrtc_peer_t *peer,
                                                  h2_pal_result_t result) {
    if (result == H2_PAL_OK) {
        result = H2_PAL_ERR_IO;
    }
    peer->stream_reset_failure = result;
    h2_peer_terminal_all_channels(peer, H2_PAL_WEBRTC_CHANNEL_ERROR);
    h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_FAILED);
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        h2_peer_portable_peer_close(peer);
    } else {
        h2_peer_close_providers(peer);
    }
    return result;
}

static h2_pal_result_t
h2_peer_service_stream_resets(h2_pal_webrtc_peer_t *peer) {
    if (peer->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (peer->stream_reset_failure != H2_PAL_OK) {
        return peer->stream_reset_failure;
    }
    int reset_in_flight = 0;
    for (size_t i = 0u; i < H2_PEER_STREAM_COUNT; ++i) {
        h2_peer_stream_reset_t *reset = &peer->stream_resets[i];
        if (!reset->active) {
            continue;
        }
        if (reset->outgoing_completed && reset->incoming_reset) {
            h2_pal_result_t result = h2_peer_forget_stream(peer, (uint16_t)i);
            if (result != H2_PAL_OK) {
                return h2_peer_fail_stream_resets(peer, result);
            }
            uint32_t generation = reset->generation;
            memset(reset, 0, sizeof(*reset));
            reset->generation = generation;
            continue;
        }
        if (reset->outgoing_submitted && !reset->outgoing_completed) {
            reset_in_flight = 1;
        }
    }
    if (reset_in_flight) {
        return H2_PAL_OK;
    }
    for (size_t i = 0u; i < H2_PEER_STREAM_COUNT; ++i) {
        h2_peer_stream_reset_t *reset = &peer->stream_resets[i];
        if (!reset->active || reset->outgoing_submitted) {
            continue;
        }
        h2_pal_result_t result = h2_peer_submit_stream_reset(peer, (uint16_t)i);
        if (result == H2_PAL_OK) {
            reset->outgoing_submitted = 1;
            return H2_PAL_OK;
        }
        if (result == H2_PAL_ERR_BUSY || result == H2_PAL_ERR_WOULD_BLOCK) {
            return H2_PAL_OK;
        }
        return h2_peer_fail_stream_resets(peer, result);
    }
    if (!h2_peer_has_active_stream_reset(peer)) {
        return h2_peer_open_pending_channels(peer);
    }
    return H2_PAL_OK;
}

void h2_peer_webrtc_on_stream_reset(
    h2_pal_webrtc_peer_t *peer, const h2_pal_sctp_stream_reset_event_t *event) {
    if (peer == NULL || event == NULL || peer->closed) {
        return;
    }
    if (event->stream_id >= H2_PEER_STREAM_COUNT) {
        peer->stream_reset_failure = H2_PAL_ERR_INVALID_ARG;
        return;
    }
    if (event->result != H2_PAL_OK) {
        peer->stream_reset_failure = event->result;
        return;
    }
    h2_peer_stream_reset_t *reset = &peer->stream_resets[event->stream_id];
    h2_pal_webrtc_channel_t *channel =
        h2_peer_find_channel(peer, event->stream_id);
    if (!reset->active) {
        if (event->direction != H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET ||
            channel == NULL) {
            return;
        }
        reset->active = 1;
        reset->generation = channel->generation;
    }
    if (event->direction == H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED) {
        if (!reset->outgoing_submitted) {
            return;
        }
        reset->outgoing_completed = 1;
    } else if (event->direction == H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET) {
        reset->incoming_reset = 1;
        if (channel != NULL && channel->generation == reset->generation) {
            h2_peer_terminal_channel(channel, H2_PAL_WEBRTC_CHANNEL_CLOSED);
        }
    } else {
        peer->stream_reset_failure = H2_PAL_ERR_INVALID_ARG;
    }
}

void h2_peer_webrtc_on_sctp_closed(h2_pal_webrtc_peer_t *peer) {
    if (peer == NULL || peer->closed) {
        return;
    }
    h2_peer_terminal_all_channels(peer, H2_PAL_WEBRTC_CHANNEL_CLOSED);
}

static void h2_peer_sctp_event(void *user, const h2_peer_sctp_event_t *event) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)user;
    if (peer == NULL || event == NULL || peer->closed) {
        return;
    }
    if (event->type == H2_PEER_SCTP_EVENT_STREAM_RESET) {
        const h2_pal_sctp_stream_reset_event_t reset_event = {
            .stream_id = event->stream_id,
            .direction = event->reset_direction,
            .result = event->reset_result,
        };
        h2_peer_webrtc_on_stream_reset(peer, &reset_event);
        return;
    }
    h2_pal_webrtc_channel_t *channel =
        h2_peer_find_channel(peer, event->stream_id);
    if (channel == NULL) {
        return;
    }
    if (event->type == H2_PEER_SCTP_EVENT_CHANNEL_OPEN && !channel->open) {
        channel->open = 1;
        h2_peer_webrtc_emit_channel_state(channel, H2_PAL_WEBRTC_CHANNEL_OPEN);
    } else if (event->type == H2_PEER_SCTP_EVENT_CHANNEL_CLOSED) {
        const h2_pal_sctp_stream_reset_event_t reset_event = {
            .stream_id = event->stream_id,
            .direction = H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET,
            .result = H2_PAL_OK,
        };
        h2_peer_webrtc_on_stream_reset(peer, &reset_event);
    } else if (event->type == H2_PEER_SCTP_EVENT_CHANNEL_MESSAGE &&
               channel->open &&
               (event->data != NULL || event->data_len == 0u)) {
        h2_peer_webrtc_emit_channel_message(peer, channel, event->data,
                                            event->data_len, event->is_text);
    }
}

static void h2_peer_close_providers(h2_pal_webrtc_peer_t *peer) {
    h2_peer_t *owner = peer->owner;
    if (peer->sctp_open) {
        owner->providers.sctp.vtable->close(owner->providers.sctp.user,
                                            peer->sctp_session);
        peer->sctp_open = 0;
        peer->sctp_session = NULL;
    }
    if (peer->srtp_open) {
        owner->providers.srtp.vtable->close(owner->providers.srtp.user,
                                            peer->srtp_session);
        peer->srtp_open = 0;
        peer->srtp_session = NULL;
    }
    if (peer->dtls_open) {
        owner->providers.dtls.vtable->close(owner->providers.dtls.user,
                                            peer->dtls_session);
        peer->dtls_open = 0;
        peer->dtls_session = NULL;
    }
    if (peer->ice_open) {
        owner->providers.ice.vtable->close(owner->providers.ice.user,
                                           peer->ice_session);
        peer->ice_open = 0;
        peer->ice_session = NULL;
    }
}

static void h2_peer_mark_channels_closed(h2_pal_webrtc_peer_t *peer) {
    h2_peer_terminal_all_channels(peer, H2_PAL_WEBRTC_CHANNEL_CLOSED);
}

static h2_pal_result_t h2_peer_open_providers(h2_pal_webrtc_peer_t *peer) {
    h2_peer_t *owner = peer->owner;
    h2_pal_webrtc_ice_server_t servers[H2_PEER_ICE_SERVER_MAX];
    for (size_t i = 0u; i < peer->ice_server_count; ++i) {
        servers[i].url.data = peer->ice_servers[i].url;
        servers[i].url.len = strlen(peer->ice_servers[i].url);
        servers[i].username.data = peer->ice_servers[i].username;
        servers[i].username.len = peer->ice_servers[i].username == NULL
                                      ? 0u
                                      : strlen(peer->ice_servers[i].username);
        servers[i].credential.data = peer->ice_servers[i].credential;
        servers[i].credential.len =
            peer->ice_servers[i].credential == NULL
                ? 0u
                : strlen(peer->ice_servers[i].credential);
    }
    h2_pal_result_t result = owner->providers.ice.vtable->open(
        owner->providers.ice.user,
        peer->ice_server_count == 0u ? NULL : servers, peer->ice_server_count,
        &peer->ice_session);
    if (result != H2_PAL_OK) {
        return result;
    }
    peer->ice_open = 1;
    result = owner->providers.dtls.vtable->open(owner->providers.dtls.user,
                                                &peer->dtls_session);
    if (result != H2_PAL_OK) {
        h2_peer_close_providers(peer);
        return result;
    }
    peer->dtls_open = 1;
    result = owner->providers.srtp.vtable->open(owner->providers.srtp.user,
                                                &peer->srtp_session);
    if (result != H2_PAL_OK) {
        h2_peer_close_providers(peer);
        return result;
    }
    peer->srtp_open = 1;
    result = owner->providers.sctp.vtable->open(owner->providers.sctp.user,
                                                &peer->sctp_session);
    if (result != H2_PAL_OK) {
        h2_peer_close_providers(peer);
        return result;
    }
    peer->sctp_open = 1;
    return H2_PAL_OK;
}

static void h2_peer_close_now(h2_pal_webrtc_peer_t *peer);

static void h2_peer_finish_destroy(h2_peer_t *owner) {
    if (!owner->destroying || owner->operation_depth != 0u) {
        return;
    }
    owner->operation_depth++;
    while (owner->peers != NULL) {
        h2_peer_close_now(owner->peers);
    }
    while (owner->tracks != NULL) {
        h2_pal_webrtc_track_t *track = owner->tracks;
        owner->tracks = track->next;
        h2_peer_free(owner, track);
    }
    owner->operation_depth--;
    h2_peer_control_free(owner, owner);
}

static void h2_peer_begin_operation(h2_pal_webrtc_peer_t *peer) {
    peer->operation_depth++;
    peer->owner->operation_depth++;
}

static h2_pal_result_t h2_peer_end_operation(h2_pal_webrtc_peer_t *peer,
                                             h2_pal_result_t result) {
    h2_peer_t *owner = peer->owner;
    peer->operation_depth--;
    if (peer->operation_depth == 0u && peer->close_pending) {
        h2_peer_close_now(peer);
        result = H2_PAL_ERR_CLOSED;
    }
    owner->operation_depth--;
    h2_peer_finish_destroy(owner);
    return result;
}

static h2_pal_result_t h2_peer_webrtc_peer_create_with_flags(
    void *user, const h2_pal_webrtc_callbacks_t *callbacks,
    uint32_t receive_flags, h2_pal_webrtc_peer_t **out_peer) {
    h2_peer_t *owner = (h2_peer_t *)user;
    *out_peer = NULL;
    if (owner == NULL || owner->destroying) {
        return H2_PAL_ERR_CLOSED;
    }
    h2_pal_webrtc_peer_t *peer =
        (h2_pal_webrtc_peer_t *)h2_peer_control_alloc(owner, sizeof(*peer));
    if (peer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    atomic_init(&peer->closed, 0);
    atomic_init(&peer->operation_depth, 0u);
    atomic_init(&peer->close_pending, 0);
    peer->owner = owner;
    peer->callbacks = *callbacks;
    peer->receive_flags = receive_flags;
    atomic_init(&peer->state, H2_PAL_WEBRTC_PEER_NEW);
    peer->local_stream_first = owner->production_backend ? 1u : 0u;
    peer->next_stream_id = peer->local_stream_first;
    peer->rtp_ssrc = UINT32_C(0x48325052);
    peer->next = owner->peers;
    owner->peers = peer;
    *out_peer = peer;
    return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_webrtc_peer_create(void *user,
                           const h2_pal_webrtc_callbacks_t *callbacks,
                           h2_pal_webrtc_peer_t **out_peer) {
    return h2_peer_webrtc_peer_create_with_flags(user, callbacks, 0u, out_peer);
}

static h2_pal_result_t
h2_peer_webrtc_add_ice_server(h2_pal_webrtc_peer_t *peer,
                              const h2_pal_webrtc_ice_server_t *server) {
    if (peer->closed || peer->offer_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t result = h2_peer_ice_validate_server(server);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (peer->ice_server_count == H2_PEER_ICE_SERVER_MAX) {
        return H2_PAL_ERR_FULL;
    }
    h2_peer_ice_server_t copied = {0};
    copied.url = h2_peer_copy_string(peer->owner, server->url);
    if (copied.url == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    copied.username = h2_peer_copy_string(peer->owner, server->username);
    if (server->username.len != 0u && copied.username == NULL) {
        h2_peer_free(peer->owner, copied.url);
        return H2_PAL_ERR_NO_MEMORY;
    }
    copied.credential = h2_peer_copy_string(peer->owner, server->credential);
    if (server->credential.len != 0u && copied.credential == NULL) {
        h2_peer_free(peer->owner, copied.username);
        h2_peer_free(peer->owner, copied.url);
        return H2_PAL_ERR_NO_MEMORY;
    }
    peer->ice_servers[peer->ice_server_count++] = copied;
    return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_open_pending_channels(h2_pal_webrtc_peer_t *peer) {
    for (;;) {
        h2_pal_webrtc_channel_t *channel = peer->channels;
        while (channel != NULL &&
               (channel->remote_created || channel->wire_opened)) {
            channel = channel->next;
        }
        if (channel == NULL) {
            return H2_PAL_OK;
        }
        const uint16_t stream_id = channel->info.stream_id;
        const uint32_t generation = channel->generation;
        h2_pal_result_t result;
        if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
            if (!peer->production_sctp_open) {
                return H2_PAL_OK;
            }
            result = h2_peer_portable_channel_open(channel);
        } else {
            if (!peer->sctp_open) {
                return H2_PAL_OK;
            }
            result = peer->owner->providers.sctp.vtable->channel_open(
                peer->owner->providers.sctp.user, peer->sctp_session, stream_id,
                channel->info.label, channel->info.ordered,
                channel->info.reliable);
        }
        if (result != H2_PAL_OK) {
            return result;
        }
        if (peer->closed ||
            !h2_peer_channel_is_current(peer, channel, stream_id, generation)) {
            return H2_PAL_ERR_CLOSED;
        }
        channel->wire_opened = 1;
    }
}

h2_pal_result_t h2_peer_webrtc_on_remote_channel_open(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_str_t label, uint16_t stream_id,
    int ordered, int reliable) {
    if (peer == NULL || peer->closed || label.data == NULL || label.len == 0u ||
        label.len > H2_PEER_CHANNEL_LABEL_MAX ||
        stream_id >= H2_PEER_STREAM_COUNT ||
        (stream_id & 1u) == (peer->local_stream_first & 1u) ||
        h2_peer_find_channel(peer, stream_id) != NULL ||
        peer->stream_resets[stream_id].active) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_webrtc_channel_t *channel =
        (h2_pal_webrtc_channel_t *)h2_peer_control_alloc(peer->owner,
                                                         sizeof(*channel));
    if (channel == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    channel->ready_slot = UINT8_MAX;
    atomic_init(&channel->callback_refs, 0u);
    atomic_init(&channel->open, 0);
    atomic_init(&channel->terminal, 0);
    atomic_init(&channel->tx_state[0], 0u);
    for (size_t i = 0u; i < H2_PEER_OUTPUT_SLOT_COUNT; ++i) {
        atomic_init(&channel->rx_state[i], 0u);
    }
    atomic_init(&channel->rx_count, 0u);
    channel->label = h2_peer_copy_string(peer->owner, label);
    if (channel->label == NULL) {
        h2_peer_control_free(peer->owner, channel);
        return H2_PAL_ERR_NO_MEMORY;
    }
    channel->owner = peer;
    if (h2_peer_network_enabled(peer) && h2_peer_channel_pull_enabled(peer)) {
        h2_pal_result_t gate_result = h2_peer_receive_gate_create(
            peer, "h2peer/channel/rx", &channel->rx_gate);
        if (gate_result != H2_PAL_OK) {
            h2_peer_free(peer->owner, channel->label);
            h2_peer_control_free(peer->owner, channel);
            return gate_result;
        }
    }
    if (h2_peer_network_enabled(peer)) {
        h2_pal_result_t ready_result =
            h2_peer_channel_ready_slot_allocate(channel);
        if (ready_result != H2_PAL_OK) {
            h2_pal_queue_destroy(peer->owner->config.queue, channel->rx_gate);
            h2_peer_free(peer->owner, channel->label);
            h2_peer_control_free(peer->owner, channel);
            return ready_result;
        }
    }
    channel->info.label.data = channel->label;
    channel->info.label.len = label.len;
    channel->info.stream_id = stream_id;
    channel->info.has_stream_id = 1;
    channel->info.ordered = ordered != 0;
    channel->info.reliable = reliable != 0;
    uint32_t generation = peer->stream_resets[stream_id].generation + 1u;
    if (generation == 0u) {
        generation = 1u;
    }
    peer->stream_resets[stream_id].generation = generation;
    channel->generation = generation;
    channel->open = 1;
    channel->wire_opened = 1;
    channel->remote_created = 1;
    channel->next = peer->channels;
    peer->channels = channel;
    h2_peer_webrtc_emit_channel_state(channel, H2_PAL_WEBRTC_CHANNEL_OPEN);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_peer_webrtc_start_offer(h2_pal_webrtc_peer_t *peer) {
    if (peer->closed || peer->offer_started ||
        peer->state != H2_PAL_WEBRTC_PEER_NEW) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_peer_begin_operation(peer);
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        h2_pal_result_t production_result = h2_peer_portable_start_offer(peer);
        if (production_result != H2_PAL_OK) {
            h2_peer_portable_peer_close(peer);
        }
        return h2_peer_end_operation(peer, production_result);
    }
    h2_pal_result_t result = h2_peer_open_providers(peer);
    if (result != H2_PAL_OK) {
        return h2_peer_end_operation(peer, result);
    }
    result = h2_peer_open_pending_channels(peer);
    if (result != H2_PAL_OK) {
        h2_peer_close_providers(peer);
        return h2_peer_end_operation(peer, result);
    }
    uint8_t random[16];
    result = (h2_pal_result_t)h2_pal_crypto_random(peer->owner->config.crypto,
                                                   random, sizeof(random));
    if (result != H2_PAL_OK) {
        h2_peer_close_providers(peer);
        return h2_peer_end_operation(peer, result);
    }
    peer->rtp_ssrc = ((uint32_t)random[0] << 24u) |
                     ((uint32_t)random[1] << 16u) |
                     ((uint32_t)random[2] << 8u) | random[3];
    char sdp[H2_PEER_SDP_MAX];
    char fingerprint[96];
    size_t fingerprint_len = 0u;
    size_t sdp_len = 0u;
    result = peer->owner->providers.dtls.vtable->get_local_fingerprint(
        peer->owner->providers.dtls.user, peer->dtls_session, fingerprint,
        sizeof(fingerprint), &fingerprint_len);
    if (result != H2_PAL_OK || fingerprint_len == 0u ||
        fingerprint_len > sizeof(fingerprint) - 1u) {
        h2_peer_close_providers(peer);
        return h2_peer_end_operation(
            peer, result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result);
    }
    h2_pal_webrtc_str_t fingerprint_value = {
        .data = fingerprint,
        .len = fingerprint_len,
    };
    result = h2_peer_sdp_write_offer(random, peer->rtp_ssrc, fingerprint_value,
                                     sdp, sizeof(sdp), &sdp_len);
    if (result != H2_PAL_OK) {
        h2_peer_close_providers(peer);
        return h2_peer_end_operation(peer, result);
    }
    peer->offer_started = 1;
    h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CONNECTING);
    if (!peer->closed) {
        h2_pal_webrtc_str_t value = {.data = sdp, .len = sdp_len};
        h2_peer_webrtc_emit_local_sdp(peer, H2_PAL_WEBRTC_SDP_OFFER, value);
    }
    return h2_peer_end_operation(peer,
                                 peer->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK);
}

static h2_pal_result_t
h2_peer_webrtc_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                              h2_pal_webrtc_sdp_type_t type,
                              h2_pal_webrtc_str_t sdp) {
    if (peer->closed || !peer->offer_started || peer->remote_answer_set ||
        type != H2_PAL_WEBRTC_SDP_ANSWER) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        h2_peer_begin_operation(peer);
        h2_pal_result_t production_result =
            h2_peer_portable_set_remote_sdp(peer, type, sdp);
        return h2_peer_end_operation(peer, production_result);
    }
    h2_peer_sdp_description_t description;
    h2_pal_result_t result = h2_peer_sdp_parse(sdp, &description);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (!description.has_opus && !description.has_data_channel) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    result = peer->owner->providers.dtls.vtable->set_remote_fingerprint(
        peer->owner->providers.dtls.user, peer->dtls_session,
        description.fingerprint);
    if (result != H2_PAL_OK) {
        return result;
    }
    peer->remote_answer_set = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_webrtc_create_data_channel(h2_pal_webrtc_peer_t *peer,
                                   const h2_pal_webrtc_channel_config_t *config,
                                   h2_pal_webrtc_channel_t **out_channel) {
    *out_channel = NULL;
    if (peer->closed || peer->state == H2_PAL_WEBRTC_PEER_FAILED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->label.len > H2_PEER_CHANNEL_LABEL_MAX) {
        return H2_PAL_ERR_NO_SPACE;
    }
    uint16_t stream_id = config->stream_id;
    size_t stream_slot = 0u;
    if (config->has_stream_id) {
        if (!h2_peer_stream_slot(peer, stream_id, &stream_slot) ||
            h2_peer_find_channel(peer, stream_id) != NULL ||
            peer->stream_resets[stream_id].active) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    } else {
        size_t first_slot = 0u;
        if (!h2_peer_stream_slot(peer, peer->next_stream_id, &first_slot)) {
            first_slot = 0u;
        }
        int found = 0;
        for (size_t i = 0u; i < H2_PEER_LOCAL_STREAM_COUNT; ++i) {
            size_t candidate_slot =
                (first_slot + i) % H2_PEER_LOCAL_STREAM_COUNT;
            uint16_t candidate =
                h2_peer_stream_id_for_slot(peer, candidate_slot);
            if (h2_peer_find_channel(peer, candidate) == NULL &&
                !peer->stream_resets[candidate].active) {
                stream_id = candidate;
                stream_slot = candidate_slot;
                found = 1;
                break;
            }
        }
        if (!found) {
            return H2_PAL_ERR_NO_SPACE;
        }
    }
    h2_pal_webrtc_channel_t *channel =
        (h2_pal_webrtc_channel_t *)h2_peer_control_alloc(peer->owner,
                                                         sizeof(*channel));
    if (channel == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    channel->ready_slot = UINT8_MAX;
    atomic_init(&channel->callback_refs, 0u);
    atomic_init(&channel->open, 0);
    atomic_init(&channel->terminal, 0);
    atomic_init(&channel->tx_state[0], 0u);
    for (size_t i = 0u; i < H2_PEER_OUTPUT_SLOT_COUNT; ++i) {
        atomic_init(&channel->rx_state[i], 0u);
    }
    atomic_init(&channel->rx_count, 0u);
    channel->label = h2_peer_copy_string(peer->owner, config->label);
    if (channel->label == NULL) {
        h2_peer_control_free(peer->owner, channel);
        return H2_PAL_ERR_NO_MEMORY;
    }
    channel->owner = peer;
    if (h2_peer_network_enabled(peer) && h2_peer_channel_pull_enabled(peer)) {
        h2_pal_result_t gate_result = h2_peer_receive_gate_create(
            peer, "h2peer/channel/rx", &channel->rx_gate);
        if (gate_result != H2_PAL_OK) {
            h2_peer_free(peer->owner, channel->label);
            h2_peer_control_free(peer->owner, channel);
            return gate_result;
        }
    }
    if (h2_peer_network_enabled(peer)) {
        h2_pal_result_t ready_result =
            h2_peer_channel_ready_slot_allocate(channel);
        if (ready_result != H2_PAL_OK) {
            h2_pal_queue_destroy(peer->owner->config.queue, channel->rx_gate);
            h2_peer_free(peer->owner, channel->label);
            h2_peer_control_free(peer->owner, channel);
            return ready_result;
        }
    }
    channel->info.label.data = channel->label;
    channel->info.label.len = config->label.len;
    channel->info.stream_id = stream_id;
    channel->info.has_stream_id = 1;
    channel->info.ordered = config->ordered != 0;
    channel->info.reliable = config->reliable != 0;
    uint32_t generation = peer->stream_resets[stream_id].generation + 1u;
    if (generation == 0u) {
        generation = 1u;
    }
    peer->stream_resets[stream_id].generation = generation;
    channel->generation = generation;
    channel->next = peer->channels;
    peer->channels = channel;
    if (!config->has_stream_id) {
        size_t next_slot = (stream_slot + 1u) % H2_PEER_LOCAL_STREAM_COUNT;
        peer->next_stream_id = h2_peer_stream_id_for_slot(peer, next_slot);
    }
    h2_peer_begin_operation(peer);
    h2_pal_result_t result = H2_PAL_OK;
    const uint32_t channel_generation = channel->generation;
    int attempted_open = 0;
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner) &&
        peer->production_sctp_open) {
        attempted_open = 1;
        result = h2_peer_portable_channel_open(channel);
    } else if (peer->sctp_open) {
        attempted_open = 1;
        result = peer->owner->providers.sctp.vtable->channel_open(
            peer->owner->providers.sctp.user, peer->sctp_session, stream_id,
            channel->info.label, channel->info.ordered, channel->info.reliable);
    }
    const int channel_current = h2_peer_channel_is_current(
        peer, channel, stream_id, channel_generation);
    if (result != H2_PAL_OK) {
        if (channel_current) {
            h2_peer_unlink_channel(peer, channel);
            h2_peer_free_channel(peer, channel);
        }
        (void)h2_peer_end_operation(peer, result);
        return result;
    }
    if (peer->closed || !channel_current) {
        return h2_peer_end_operation(peer, H2_PAL_ERR_CLOSED);
    }
    if (attempted_open) {
        channel->wire_opened = 1;
    }
    result = h2_peer_end_operation(peer, H2_PAL_OK);
    if (result == H2_PAL_OK) {
        *out_channel = channel;
    }
    return result;
}

static h2_pal_result_t
h2_peer_webrtc_set_media_track(h2_pal_webrtc_peer_t *peer,
                               h2_pal_webrtc_track_t *track) {
    if (peer == NULL || peer->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (peer->offer_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (track != NULL &&
        (track->owner != peer->owner ||
         (track->bound_peer != NULL && track->bound_peer != peer))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (track != NULL && (h2_peer_opus_pull_enabled(peer) ||
                          peer->callbacks.on_opus_frame != NULL)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (peer->media_track != NULL)
        peer->media_track->bound_peer = NULL;
    peer->media_track = track;
    if (track != NULL)
        track->bound_peer = peer;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_peer_remaining_timeout(h2_pal_webrtc_peer_t *peer,
                                                 uint64_t start_ms,
                                                 int timeout_ms,
                                                 int *out_timeout_ms) {
    uint64_t now_ms = 0u;
    h2_pal_result_t result =
        h2_pal_time_get_monotonic_ms(peer->owner->config.time, &now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    uint64_t elapsed_ms = h2_pal_time_elapsed_ms(start_ms, now_ms);
    *out_timeout_ms =
        elapsed_ms >= (uint64_t)timeout_ms ? 0 : timeout_ms - (int)elapsed_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_peer_poll_terminal_error(h2_pal_webrtc_peer_t *peer,
                                                   h2_pal_result_t result) {
    h2_peer_webrtc_emit_peer_state(peer, result == H2_PAL_ERR_CLOSED
                                             ? H2_PAL_WEBRTC_PEER_DISCONNECTED
                                             : H2_PAL_WEBRTC_PEER_FAILED);
    h2_peer_mark_channels_closed(peer);
    h2_peer_close_providers(peer);
    return result;
}

static h2_pal_result_t h2_peer_webrtc_poll(h2_pal_webrtc_peer_t *peer,
                                           int timeout_ms) {
    if (peer->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (!peer->offer_started || !peer->remote_answer_set || timeout_ms < 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        h2_peer_begin_operation(peer);
        h2_pal_result_t production_result = h2_peer_service_stream_resets(peer);
        if (production_result == H2_PAL_OK) {
            production_result = h2_peer_portable_poll(peer, timeout_ms);
        }
        h2_pal_result_t reset_result = h2_peer_service_stream_resets(peer);
        if (reset_result != H2_PAL_OK) {
            production_result = reset_result;
        }
        return h2_peer_end_operation(peer, production_result);
    }
    if (!peer->ice_open || !peer->dtls_open || !peer->sctp_open) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    h2_peer_begin_operation(peer);
    h2_pal_result_t result = h2_peer_service_stream_resets(peer);
    if (result != H2_PAL_OK) {
        return h2_peer_end_operation(peer, result);
    }
    uint64_t start_ms = 0u;
    result = h2_pal_time_get_monotonic_ms(peer->owner->config.time, &start_ms);
    if (result != H2_PAL_OK) {
        return h2_peer_end_operation(peer, result);
    }
    int remaining_ms = timeout_ms;
    result = peer->owner->providers.ice.vtable->poll(
        peer->owner->providers.ice.user, peer->ice_session, remaining_ms);
    if (peer->closed) {
        return h2_peer_end_operation(peer, H2_PAL_ERR_CLOSED);
    }
    if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
        return h2_peer_end_operation(peer, result);
    }
    if (result != H2_PAL_OK) {
        result = h2_peer_poll_terminal_error(peer, result);
        return h2_peer_end_operation(peer, result);
    }

    result =
        h2_peer_remaining_timeout(peer, start_ms, timeout_ms, &remaining_ms);
    if (result != H2_PAL_OK) {
        return h2_peer_end_operation(peer, result);
    }
    result = peer->owner->providers.dtls.vtable->poll(
        peer->owner->providers.dtls.user, peer->dtls_session, remaining_ms);
    if (peer->closed) {
        return h2_peer_end_operation(peer, H2_PAL_ERR_CLOSED);
    }
    if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
        return h2_peer_end_operation(peer, result);
    }
    if (result != H2_PAL_OK) {
        result = h2_peer_poll_terminal_error(peer, result);
        return h2_peer_end_operation(peer, result);
    }

    result =
        h2_peer_remaining_timeout(peer, start_ms, timeout_ms, &remaining_ms);
    if (result != H2_PAL_OK) {
        return h2_peer_end_operation(peer, result);
    }
    result = peer->owner->providers.sctp.vtable->poll(
        peer->owner->providers.sctp.user, peer->sctp_session, remaining_ms,
        h2_peer_sctp_event, peer);
    h2_pal_result_t reset_result = h2_peer_service_stream_resets(peer);
    if (reset_result != H2_PAL_OK) {
        return h2_peer_end_operation(peer, reset_result);
    }
    if (peer->closed) {
        return h2_peer_end_operation(peer, H2_PAL_ERR_CLOSED);
    }
    if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
        return h2_peer_end_operation(peer, result);
    }
    if (result != H2_PAL_OK) {
        result = h2_peer_poll_terminal_error(peer, result);
        return h2_peer_end_operation(peer, result);
    }
    h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CONNECTED);
    return h2_peer_end_operation(peer,
                                 peer->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK);
}

static h2_pal_result_t h2_peer_webrtc_send_opus(h2_pal_webrtc_peer_t *peer,
                                                const uint8_t *opus,
                                                size_t opus_len) {
    if (peer->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        return h2_peer_portable_send_opus(peer, opus, opus_len);
    }
    if (peer->state != H2_PAL_WEBRTC_PEER_CONNECTED || !peer->srtp_open) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint8_t
        packet[H2_PEER_RTP_HEADER_SIZE + H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
    size_t packet_len = 0u;
    h2_pal_result_t result = h2_peer_rtp_write_opus(
        peer->rtp_sequence, peer->rtp_timestamp, peer->rtp_ssrc, opus, opus_len,
        packet, sizeof(packet), &packet_len);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = peer->owner->providers.srtp.vtable->send_rtp(
        peer->owner->providers.srtp.user, peer->srtp_session, packet,
        packet_len);
    if (result == H2_PAL_OK) {
        peer->rtp_sequence++;
        peer->rtp_timestamp += 960u;
    }
    return result;
}

static h2_pal_result_t
h2_peer_webrtc_channel_send(h2_pal_webrtc_channel_t *channel,
                            const uint8_t *data, size_t len, int is_text) {
    h2_pal_webrtc_peer_t *peer = channel->owner;
    if (peer->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (!channel->open || peer->state != H2_PAL_WEBRTC_PEER_CONNECTED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        return h2_peer_portable_channel_send(channel, data, len, is_text);
    }
    return peer->owner->providers.sctp.vtable->send(
        peer->owner->providers.sctp.user, peer->sctp_session,
        channel->info.stream_id, data, len, is_text != 0);
}

static void h2_peer_webrtc_channel_close(h2_pal_webrtc_channel_t *channel) {
    if (channel == NULL || channel->terminal) {
        return;
    }
    h2_pal_webrtc_peer_t *peer = channel->owner;
    h2_peer_begin_operation(peer);
    if (channel->wire_opened) {
        uint16_t stream_id = channel->info.stream_id;
        if (stream_id >= H2_PEER_STREAM_COUNT) {
            peer->stream_reset_failure = H2_PAL_ERR_INVALID_ARG;
        } else {
            h2_peer_stream_reset_t *reset = &peer->stream_resets[stream_id];
            reset->active = 1;
            reset->generation = channel->generation;
        }
    }
    h2_peer_terminal_channel(channel, H2_PAL_WEBRTC_CHANNEL_CLOSED);
    h2_pal_result_t result = h2_peer_service_stream_resets(peer);
    (void)h2_peer_end_operation(peer, result);
}

static void h2_peer_free_channel(h2_pal_webrtc_peer_t *peer,
                                 h2_pal_webrtc_channel_t *channel) {
    h2_peer_channel_ready_clear(channel);
    if (h2_peer_network_enabled(peer) && channel->rx_gate != NULL) {
        const unsigned int receive_count = atomic_exchange_explicit(
            &channel->rx_count, 0u, memory_order_acq_rel);
        if (receive_count != 0u) {
            atomic_fetch_sub_explicit(&peer->network_receive_count,
                                      receive_count, memory_order_relaxed);
            if (receive_count == H2_PEER_RECEIVE_SLOT_COUNT) {
                atomic_fetch_sub_explicit(&peer->network_receive_full, 1u,
                                          memory_order_release);
            }
        }
    }
    for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i) {
        atomic_store_explicit(&channel->tx_state[i], 0u, memory_order_release);
        h2_peer_free_tx_item(peer->owner, channel->tx_storage[i]);
        channel->tx_storage[i] = NULL;
    }
    for (size_t i = 0u; i < H2_PEER_OUTPUT_SLOT_COUNT; ++i) {
        atomic_store_explicit(&channel->rx_state[i], 0u, memory_order_release);
        h2_peer_free_tx_item(peer->owner, channel->rx_storage[i]);
        channel->rx_storage[i] = NULL;
    }
    h2_pal_queue_destroy(peer->owner->config.queue, channel->rx_gate);
    channel->rx_gate = NULL;
    h2_peer_free(peer->owner, channel->label);
    h2_peer_control_free(peer->owner, channel);
}

static void h2_peer_free_ice_servers(h2_pal_webrtc_peer_t *peer) {
    for (size_t i = 0u; i < peer->ice_server_count; ++i) {
        h2_peer_free(peer->owner, peer->ice_servers[i].credential);
        h2_peer_free(peer->owner, peer->ice_servers[i].username);
        h2_peer_free(peer->owner, peer->ice_servers[i].url);
    }
    peer->ice_server_count = 0u;
}

static void h2_peer_close_now(h2_pal_webrtc_peer_t *peer) {
    peer->closed = 1;
    peer->close_pending = 0;
    if (H2_PEER_USES_PRODUCTION_BACKEND(peer->owner)) {
        h2_peer_portable_peer_close(peer);
    } else {
        h2_peer_close_providers(peer);
    }
    if (peer->media_track != NULL) {
        peer->media_track->bound_peer = NULL;
        peer->media_track = NULL;
    }
    h2_peer_mark_channels_closed(peer);
    while (peer->channels != NULL) {
        h2_pal_webrtc_channel_t *channel = peer->channels;
        peer->channels = channel->next;
        h2_peer_free_channel(peer, channel);
    }
    h2_peer_free_ice_servers(peer);
    h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CLOSED);

    h2_pal_webrtc_peer_t **cursor = &peer->owner->peers;
    while (*cursor != NULL && *cursor != peer) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == peer) {
        *cursor = peer->next;
    }
    if (!h2_peer_network_enabled(peer)) {
        h2_peer_control_free(peer->owner, peer);
    }
}

static void h2_peer_webrtc_peer_close(h2_pal_webrtc_peer_t *peer) {
    if (peer->operation_depth != 0u) {
        peer->closed = 1;
        peer->close_pending = 1;
        return;
    }
    if (!peer->closed || peer->close_pending) {
        h2_peer_t *owner = peer->owner;
        owner->operation_depth++;
        h2_peer_close_now(peer);
        owner->operation_depth--;
        h2_peer_finish_destroy(owner);
    }
}

static void h2_peer_network_release_event(h2_peer_network_event_t *event) {
    h2_pal_webrtc_peer_t *peer = event->peer;
    h2_pal_webrtc_channel_t *channel = event->channel;
    atomic_fetch_sub_explicit(&peer->network_event_bytes, event->data_len,
                              memory_order_relaxed);
    atomic_fetch_sub_explicit(&peer->network_event_count, 1u,
                              memory_order_relaxed);
    if (channel != NULL) {
        unsigned int previous = atomic_fetch_sub_explicit(
            &channel->callback_refs, 1u, memory_order_acq_rel);
        if (previous == (H2_PEER_CHANNEL_FREE_PENDING | 1u)) {
            h2_peer_free_channel(peer, channel);
        }
    }
    h2_peer_free(peer->owner, event);
}

static void h2_peer_network_dispatch_event(h2_peer_network_event_t *event) {
    h2_pal_webrtc_peer_t *peer = event->peer;
    h2_pal_webrtc_channel_t *channel = event->channel;
    switch (event->type) {
    case H2_PEER_NETWORK_EVENT_PEER_STATE:
        if (peer->callbacks.on_peer_state != NULL) {
            peer->callbacks.on_peer_state(peer->callbacks.user, peer,
                                          event->peer_state);
        }
        break;
    case H2_PEER_NETWORK_EVENT_LOCAL_SDP:
        if (peer->callbacks.on_local_sdp != NULL && !peer->closed) {
            const h2_pal_webrtc_str_t sdp = {
                .data = (const char *)event->data,
                .len = event->data_len,
            };
            peer->callbacks.on_local_sdp(peer->callbacks.user, peer,
                                         event->sdp_type, sdp);
        }
        break;
    case H2_PEER_NETWORK_EVENT_CHANNEL_STATE:
        if (peer->callbacks.on_channel_state != NULL) {
            peer->callbacks.on_channel_state(peer->callbacks.user, peer,
                                             channel, &channel->info,
                                             event->channel_state);
        }
        break;
    case H2_PEER_NETWORK_EVENT_CHANNEL_MESSAGE:
        if (peer->callbacks.on_channel_message != NULL && !peer->closed) {
            peer->callbacks.on_channel_message(
                peer->callbacks.user, peer, channel, &channel->info,
                event->data, event->data_len, event->is_text);
        }
        break;
    case H2_PEER_NETWORK_EVENT_OPUS_FRAME:
        if (peer->callbacks.on_opus_frame != NULL && !peer->closed) {
            peer->callbacks.on_opus_frame(peer->callbacks.user, peer,
                                          event->data, event->data_len);
        }
        break;
    case H2_PEER_NETWORK_EVENT_RECEIVE_READY:
        atomic_store_explicit(&peer->network_receive_wakeup_queued, 0,
                              memory_order_release);
        break;
    case H2_PEER_NETWORK_EVENT_SEND_READY:
        atomic_store_explicit(&peer->network_send_wakeup_queued, 0,
                              memory_order_release);
        break;
    }
    h2_peer_network_release_event(event);
}

static void
h2_peer_network_dispatch_available(h2_pal_webrtc_peer_t *peer,
                                   h2_peer_network_event_t *first_event) {
    peer->callback_dispatch_depth++;
    if (first_event != NULL) {
        h2_peer_network_dispatch_event(first_event);
    }
    for (;;) {
        h2_peer_network_event_t *event = NULL;
        h2_pal_result_t result = (h2_pal_result_t)h2_pal_queue_recv(
            peer->owner->config.queue, peer->network_events, &event,
            H2_PAL_QUEUE_NO_WAIT);
        if (result != H2_PAL_OK) {
            break;
        }
        h2_peer_network_dispatch_event(event);
    }
    peer->callback_dispatch_depth--;
}

static void h2_peer_network_destroy_resources(h2_pal_webrtc_peer_t *peer) {
    h2_peer_t *owner = peer->owner;
    if (peer->media_track != NULL) {
        peer->media_track->bound_peer = NULL;
        peer->media_track = NULL;
    }
    (void)atomic_exchange_explicit(&peer->rtp_pending, NULL,
                                   memory_order_acq_rel);
    h2_peer_free_tx_item(owner, peer->rtp_storage);
    peer->rtp_storage = NULL;
    for (size_t i = 0u; i < H2_PEER_RECEIVE_SLOT_COUNT; ++i) {
        h2_peer_free_tx_item(owner, peer->opus_rx_storage[i]);
        peer->opus_rx_storage[i] = NULL;
    }
    h2_pal_queue_destroy(owner->config.queue, peer->opus_rx_gate);
    h2_pal_queue_destroy(owner->config.queue, peer->network_events);
    h2_pal_queue_destroy(owner->config.queue, peer->network_responses);
    h2_pal_queue_destroy(owner->config.queue, peer->network_commands);
    (void)h2_pal_mutex_destroy(owner->config.sync, peer->network_request_mutex);
    peer->network_events = NULL;
    peer->opus_rx_gate = NULL;
    peer->network_responses = NULL;
    peer->network_commands = NULL;
    peer->network_request_mutex = NULL;
    peer->network_task = NULL;
    h2_peer_control_free(owner, peer);
}

static h2_pal_result_t
h2_peer_network_process_command(h2_pal_webrtc_peer_t *peer,
                                const h2_peer_network_command_t *command,
                                h2_peer_network_response_t *response) {
    response->result = H2_PAL_OK;
    response->channel = NULL;
    switch (command->type) {
    case H2_PEER_NETWORK_ADD_ICE_SERVER:
        response->result =
            h2_peer_webrtc_add_ice_server(peer, command->value.ice_server);
        break;
    case H2_PEER_NETWORK_START_OFFER:
        response->result = h2_peer_webrtc_start_offer(peer);
        break;
    case H2_PEER_NETWORK_SET_REMOTE_SDP:
        response->result =
            h2_peer_webrtc_set_remote_sdp(peer, command->value.remote_sdp.type,
                                          command->value.remote_sdp.sdp);
        break;
    case H2_PEER_NETWORK_CREATE_DATA_CHANNEL:
        response->result = h2_peer_webrtc_create_data_channel(
            peer, command->value.channel_config, &response->channel);
        break;
    case H2_PEER_NETWORK_SET_MEDIA_TRACK:
        response->result = h2_peer_webrtc_set_media_track(
            peer, command->value.media_track);
        break;
    case H2_PEER_NETWORK_CHANNEL_CLOSE:
        h2_peer_webrtc_channel_close(command->value.channel);
        break;
    case H2_PEER_NETWORK_PEER_CLOSE:
        atomic_store_explicit(&peer->network_stop, 1, memory_order_release);
        h2_peer_webrtc_peer_close(peer);
        break;
    default:
        response->result = H2_PAL_ERR_INVALID_ARG;
        break;
    }
    h2_pal_result_t event_result = (h2_pal_result_t)atomic_load_explicit(
        &peer->network_transport_result, memory_order_acquire);
    if (command->type != H2_PEER_NETWORK_CHANNEL_CLOSE &&
        command->type != H2_PEER_NETWORK_PEER_CLOSE &&
        response->result == H2_PAL_OK && event_result != H2_PAL_OK) {
        response->result = event_result;
    }
    return response->result;
}

static int h2_peer_network_pump_transport(h2_pal_webrtc_peer_t *peer,
                                          int timeout_ms,
                                          int *transport_terminal,
                                          int *out_waited) {
    *out_waited = 0;
    if (*transport_terminal || !peer->offer_started ||
        !peer->remote_answer_set || peer->closed) {
        return 0;
    }
    if (atomic_load_explicit(&peer->network_transport_result,
                             memory_order_acquire) != H2_PAL_OK) {
        *transport_terminal = 1;
        return 0;
    }
    const unsigned int event_count =
        atomic_load_explicit(&peer->network_event_count, memory_order_relaxed);
    const size_t event_bytes =
        atomic_load_explicit(&peer->network_event_bytes, memory_order_relaxed);
    const unsigned int receive_count = atomic_load_explicit(
        &peer->network_receive_count, memory_order_relaxed);
    const unsigned int receive_full =
        atomic_load_explicit(&peer->network_receive_full, memory_order_acquire);
    if (event_count >= H2_PEER_NETWORK_EVENT_HIGH_WATER ||
        event_bytes >= H2_PEER_NETWORK_EVENT_BYTES_HIGH_WATER ||
        receive_count >= H2_PEER_NETWORK_EVENT_HIGH_WATER ||
        receive_full != 0u) {
        return 0;
    }
    h2_pal_result_t result = H2_PAL_OK;
    int made_progress = 0;
    if (timeout_ms > 0) {
        *out_waited = 1;
    }
    if (h2_peer_portable_async_receive_supported(peer)) {
        h2_pal_net_addr_t addr;
        uint8_t packet[H2_PEER_WIRE_PACKET_MAX];
        int received = h2_peer_portable_receive_datagram(
            peer, &addr, packet, sizeof(packet), (uint32_t)timeout_ms);
        if (received > 0) {
            result = h2_peer_service_stream_resets(peer);
            if (result == H2_PAL_OK) {
                result = h2_peer_portable_service_datagram(peer, &addr, packet,
                                                           (size_t)received);
            }
            h2_pal_result_t reset_result = h2_peer_service_stream_resets(peer);
            if (reset_result != H2_PAL_OK) {
                result = reset_result;
            }
            made_progress = 1;
        } else if (received == 0 || received == H2_PAL_ERR_TIMEOUT ||
                   received == H2_PAL_ERR_WOULD_BLOCK) {
            result = h2_peer_service_stream_resets(peer);
            if (result == H2_PAL_OK) {
                result =
                    h2_peer_portable_service_datagram(peer, NULL, NULL, 0u);
            }
        } else {
            result = H2_PAL_ERR_IO;
        }
    } else {
        result = h2_peer_webrtc_poll(peer, timeout_ms);
    }
    if (result != H2_PAL_OK && result != H2_PAL_ERR_TIMEOUT &&
        result != H2_PAL_ERR_WOULD_BLOCK) {
        atomic_store_explicit(&peer->network_transport_result, result,
                              memory_order_release);
        *transport_terminal = 1;
    }
    return made_progress;
}

static void
h2_peer_network_send_response(h2_pal_webrtc_peer_t *peer,
                              const h2_peer_network_response_t *response) {
    (void)h2_pal_queue_send(peer->owner->config.queue, peer->network_responses,
                            response, H2_PAL_QUEUE_WAIT_FOREVER);
}

static int h2_peer_network_service_rtp(h2_pal_webrtc_peer_t *peer) {
    h2_peer_tx_item_t *item =
        atomic_load_explicit(&peer->rtp_pending, memory_order_acquire);
    if (item == NULL || item == H2_PEER_TX_RESERVED) {
        return 0;
    }
    h2_pal_result_t result =
        h2_peer_webrtc_send_opus(peer, item->data, item->len);
    if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
        return 0;
    }
    (void)atomic_exchange_explicit(&peer->rtp_pending, NULL,
                                   memory_order_acq_rel);
    h2_peer_network_notify_send_ready(peer);
    if (result != H2_PAL_OK) {
        h2_peer_record_network_event_error(peer, result);
    }
    return 1;
}

static h2_pal_webrtc_channel_t *
h2_peer_find_channel_by_ready_slot(h2_pal_webrtc_peer_t *peer,
                                   uint8_t ready_slot) {
    for (h2_pal_webrtc_channel_t *channel = peer->channels; channel != NULL;
         channel = channel->next) {
        if (channel->ready_slot == ready_slot) {
            return channel;
        }
    }
    return NULL;
}

static int h2_peer_network_service_channel(h2_pal_webrtc_peer_t *peer,
                                           uint32_t *snapshot) {
    int made_progress = 0;
    const uint8_t start_slot = peer->channel_round_robin;
    for (size_t offset = 0u; offset < H2_PEER_READY_CHANNEL_COUNT; ++offset) {
        const uint8_t ready_slot =
            (uint8_t)((start_slot + offset) % H2_PEER_READY_CHANNEL_COUNT);
        const uint32_t bit = UINT32_C(1) << ready_slot;
        if ((*snapshot & bit) == 0u) {
            continue;
        }
        h2_pal_webrtc_channel_t *channel =
            h2_peer_find_channel_by_ready_slot(peer, ready_slot);
        if (channel == NULL) {
            *snapshot &= ~bit;
            continue;
        }
        const size_t slot = 0u;
        if (atomic_load_explicit(&channel->tx_state[slot],
                                 memory_order_acquire) != 2u) {
            *snapshot &= ~bit;
            continue;
        }
        h2_peer_tx_item_t *item = channel->tx_storage[slot];
        h2_pal_result_t result = h2_peer_webrtc_channel_send(
            channel, item->data, item->len, item->is_text);
        if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
            peer->channel_round_robin = ready_slot;
            return made_progress;
        }
        *snapshot &= ~bit;
        atomic_store_explicit(&channel->tx_state[slot], 0u,
                              memory_order_release);
        h2_peer_network_notify_send_ready(peer);
        peer->channel_round_robin =
            (uint8_t)((ready_slot + 1u) % H2_PEER_READY_CHANNEL_COUNT);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_CLOSED &&
            result != H2_PAL_ERR_INVALID_STATE) {
            h2_peer_record_network_event_error(peer, result);
        }
        made_progress = 1;
    }
    return made_progress;
}

static h2_pal_result_t h2_peer_media_track_service(h2_pal_webrtc_peer_t *peer);

static void h2_peer_network_task(void *context) {
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)context;
    int transport_terminal = 0;
    uint32_t channel_snapshot = 0u;
    while (!atomic_load_explicit(&peer->network_stop, memory_order_acquire)) {
        uint64_t round_started_us = 0u;
        (void)h2_pal_time_get_monotonic_us(peer->owner->config.time,
                                           &round_started_us);
        h2_peer_network_command_t command;
        h2_pal_result_t command_result = (h2_pal_result_t)h2_pal_queue_recv(
            peer->owner->config.queue, peer->network_commands, &command,
            H2_PAL_QUEUE_NO_WAIT);
        if (command_result == H2_PAL_OK) {
            h2_peer_network_response_t response;
            (void)h2_peer_network_process_command(peer, &command, &response);
            h2_peer_network_send_response(peer, &response);
            h2_peer_network_record_round(peer, &peer->perf_command,
                                         round_started_us);
            continue;
        }

        if (!transport_terminal) {
            const h2_pal_result_t media_result =
                h2_peer_media_track_service(peer);
            if (media_result != H2_PAL_OK) {
                h2_peer_record_network_event_error(peer, media_result);
                transport_terminal = 1;
            }
        }

        int transport_progress = 0;
        int transport_waited = 0;
        for (size_t packet_count = 0u;
             packet_count < H2_PEER_NETWORK_UDP_BURST_MAX; ++packet_count) {
            int receive_waited = 0;
            const int receive_progress = h2_peer_network_pump_transport(
                peer, 0, &transport_terminal, &receive_waited);
            if (!receive_progress) {
                break;
            }
            transport_progress = 1;
        }

        if (channel_snapshot == 0u) {
            channel_snapshot = atomic_exchange_explicit(
                &peer->channel_ready, 0u, memory_order_acq_rel);
        }
        h2_peer_tx_item_t *rtp_pending =
            atomic_load_explicit(&peer->rtp_pending, memory_order_acquire);
        const int has_channel_work = channel_snapshot != 0u;
        bool writable = false;
        if (has_channel_work) {
            h2_pal_result_t writable_result =
                h2_peer_portable_sctp_is_writable(peer, &writable);
            if (writable_result == H2_PAL_ERR_INVALID_STATE ||
                writable_result == H2_PAL_ERR_CLOSED) {
                writable = false;
            } else if (writable_result != H2_PAL_OK) {
                h2_peer_record_network_event_error(peer, writable_result);
                writable = false;
            }
        }

        const int rtp_progress =
            rtp_pending != NULL && rtp_pending != H2_PEER_TX_RESERVED
                ? h2_peer_network_service_rtp(peer)
                : 0;
        const int channel_progress =
            writable ? h2_peer_network_service_channel(peer, &channel_snapshot)
                                         : 0;
        int receive_waited = 0;
        if (h2_peer_network_pump_transport(peer, H2_PEER_NETWORK_IDLE_WAIT_MS,
                                           &transport_terminal,
                &receive_waited)) {
            transport_progress = 1;
        }
        transport_waited = receive_waited;

        if (rtp_progress || channel_progress) {
            h2_peer_network_record_round(peer, &peer->perf_send,
                                         round_started_us);
            continue;
        }

        if (transport_progress) {
            h2_peer_network_record_round(peer, &peer->perf_transport,
                                         round_started_us);
            continue;
        }

        if (!transport_waited) {
            (void)h2_pal_time_sleep_ms(peer->owner->config.time,
                                       H2_PEER_NETWORK_IDLE_WAIT_MS);
        }
        h2_peer_network_record_round(peer, &peer->perf_idle, round_started_us);
    }
    atomic_store_explicit(&peer->network_stopped, 1, memory_order_release);
}

static h2_pal_result_t
h2_peer_network_call(h2_pal_webrtc_peer_t *peer,
                     const h2_peer_network_command_t *command,
                     h2_peer_network_response_t *out_response) {
    if (atomic_load_explicit(&peer->network_stopped, memory_order_acquire)) {
        return H2_PAL_ERR_CLOSED;
    }
    h2_pal_result_t result = h2_pal_mutex_lock(peer->owner->config.sync,
                                               peer->network_request_mutex);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (atomic_load_explicit(&peer->network_stop, memory_order_acquire) ||
        atomic_load_explicit(&peer->network_stopped, memory_order_acquire)) {
        (void)h2_pal_mutex_unlock(peer->owner->config.sync,
                                  peer->network_request_mutex);
        return H2_PAL_ERR_CLOSED;
    }
    result = (h2_pal_result_t)h2_pal_queue_send(peer->owner->config.queue,
                                                peer->network_commands, command,
                                                H2_PAL_QUEUE_WAIT_FOREVER);
    h2_peer_network_response_t response = {
        .result = result,
    };
    if (result == H2_PAL_OK) {
        result = (h2_pal_result_t)h2_pal_queue_recv(
            peer->owner->config.queue, peer->network_responses, &response,
            H2_PAL_QUEUE_WAIT_FOREVER);
    }
    h2_pal_result_t unlock_result = h2_pal_mutex_unlock(
        peer->owner->config.sync, peer->network_request_mutex);
    if (result == H2_PAL_OK && unlock_result != H2_PAL_OK) {
        result = unlock_result;
    }
    if (result == H2_PAL_OK) {
        result = response.result;
    }
    if (out_response != NULL) {
        *out_response = response;
    }
    return result;
}

static void h2_peer_network_unlink_created_peer(h2_pal_webrtc_peer_t *peer) {
    h2_pal_webrtc_peer_t **cursor = &peer->owner->peers;
    while (*cursor != NULL && *cursor != peer) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == peer) {
        *cursor = peer->next;
    }
}

static h2_pal_result_t h2_peer_network_init(h2_pal_webrtc_peer_t *peer) {
    h2_peer_t *owner = peer->owner;
    const h2_pal_mutex_config_t mutex_config = {
        .name = "h2peer/net/request",
        .allocator = owner->config.mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    h2_pal_result_t result = h2_pal_mutex_create(
        owner->config.sync, &mutex_config, &peer->network_request_mutex);
    if (result != H2_PAL_OK) {
        return result;
    }
    const h2_pal_queue_config_t command_config = {
        .name = "h2peer/net/commands",
        .item_size = sizeof(h2_peer_network_command_t),
        .item_count = H2_PEER_NETWORK_COMMAND_COUNT,
        .allocator = owner->config.mem,
    };
    result = (h2_pal_result_t)h2_pal_queue_create(
        owner->config.queue, &command_config, &peer->network_commands);
    if (result != H2_PAL_OK) {
        goto fail;
    }
    const h2_pal_queue_config_t response_config = {
        .name = "h2peer/net/responses",
        .item_size = sizeof(h2_peer_network_response_t),
        .item_count = 1u,
        .allocator = owner->config.mem,
    };
    result = (h2_pal_result_t)h2_pal_queue_create(
        owner->config.queue, &response_config, &peer->network_responses);
    if (result != H2_PAL_OK) {
        goto fail;
    }
    const h2_pal_queue_config_t event_config = {
        .name = "h2peer/net/events",
        .item_size = sizeof(h2_peer_network_event_t *),
        .item_count = H2_PEER_NETWORK_EVENT_COUNT,
        .allocator = owner->config.mem,
    };
    result = (h2_pal_result_t)h2_pal_queue_create(
        owner->config.queue, &event_config, &peer->network_events);
    if (result != H2_PAL_OK) {
        goto fail;
    }
    if (h2_peer_opus_pull_enabled(peer)) {
        result = h2_peer_receive_gate_create(peer, "h2peer/opus/rx",
                                             &peer->opus_rx_gate);
        if (result != H2_PAL_OK) {
            goto fail;
        }
    }
    atomic_init(&peer->network_event_count, 0u);
    atomic_init(&peer->network_event_bytes, 0u);
    atomic_init(&peer->network_receive_count, 0u);
    atomic_init(&peer->network_receive_full, 0u);
    atomic_init(&peer->network_receive_wakeup_queued, 0);
    atomic_init(&peer->network_send_wakeup_queued, 0);
    atomic_init(&peer->network_stop, 0);
    atomic_init(&peer->network_stopped, 0);
    atomic_init(&peer->network_transport_result, H2_PAL_OK);
    atomic_init(&peer->rtp_pending, NULL);
    for (size_t i = 0u; i < H2_PEER_RECEIVE_SLOT_COUNT; ++i) {
        atomic_init(&peer->opus_rx_state[i], 0u);
    }
    atomic_init(&peer->opus_rx_count, 0u);
    atomic_init(&peer->channel_ready, 0u);
    const h2_pal_task_options_t task_options = {
        .name = "h2peer/net",
        .min_stack_size = H2_PEER_NETWORK_STACK_SIZE,
    };
    result = h2_pal_task_start(owner->config.task, &task_options,
                               h2_peer_network_task, peer, &peer->network_task);
    if (result == H2_PAL_OK) {
        return H2_PAL_OK;
    }

fail:
    h2_pal_queue_destroy(owner->config.queue, peer->opus_rx_gate);
    h2_pal_queue_destroy(owner->config.queue, peer->network_events);
    h2_pal_queue_destroy(owner->config.queue, peer->network_responses);
    h2_pal_queue_destroy(owner->config.queue, peer->network_commands);
    (void)h2_pal_mutex_destroy(owner->config.sync, peer->network_request_mutex);
    peer->network_events = NULL;
    peer->opus_rx_gate = NULL;
    peer->network_responses = NULL;
    peer->network_commands = NULL;
    peer->network_request_mutex = NULL;
    return result;
}

static h2_pal_result_t
h2_peer_network_peer_create(void *user,
                            const h2_pal_webrtc_callbacks_t *callbacks,
                            h2_pal_webrtc_peer_t **out_peer) {
    h2_peer_t *owner = (h2_peer_t *)user;
    h2_pal_result_t result =
        h2_peer_webrtc_peer_create(user, callbacks, out_peer);
    if (result != H2_PAL_OK || !H2_PEER_USES_PRODUCTION_BACKEND(owner)) {
        return result;
    }
    result = h2_peer_network_init(*out_peer);
    if (result == H2_PAL_OK) {
        return H2_PAL_OK;
    }
    h2_pal_webrtc_peer_t *peer = *out_peer;
    *out_peer = NULL;
    h2_peer_network_unlink_created_peer(peer);
    h2_peer_control_free(owner, peer);
    return result;
}

static h2_pal_result_t h2_peer_network_peer_create_pull(
    void *user, const h2_pal_webrtc_callbacks_t *callbacks,
    uint32_t receive_flags, h2_pal_webrtc_peer_t **out_peer) {
    h2_peer_t *owner = (h2_peer_t *)user;
    if (owner == NULL || !H2_PEER_USES_PRODUCTION_BACKEND(owner)) {
        *out_peer = NULL;
        return H2_PAL_ERR_UNSUPPORTED;
    }
    const uint32_t valid_flags =
        H2_PAL_WEBRTC_RECEIVE_CHANNEL_PULL | H2_PAL_WEBRTC_RECEIVE_OPUS_PULL;
    if ((receive_flags & ~valid_flags) != 0u ||
        ((receive_flags & H2_PAL_WEBRTC_RECEIVE_CHANNEL_PULL) != 0u &&
         callbacks->on_channel_message != NULL) ||
        ((receive_flags & H2_PAL_WEBRTC_RECEIVE_OPUS_PULL) != 0u &&
         callbacks->on_opus_frame != NULL)) {
        *out_peer = NULL;
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = h2_peer_webrtc_peer_create_with_flags(
        user, callbacks, receive_flags, out_peer);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = h2_peer_network_init(*out_peer);
    if (result == H2_PAL_OK) {
        return H2_PAL_OK;
    }
    h2_peer_network_unlink_created_peer(*out_peer);
    h2_peer_control_free(owner, *out_peer);
    *out_peer = NULL;
    return result;
}

static h2_pal_result_t
h2_peer_network_add_ice_server(h2_pal_webrtc_peer_t *peer,
                               const h2_pal_webrtc_ice_server_t *server) {
    if (!h2_peer_network_enabled(peer)) {
        return h2_peer_webrtc_add_ice_server(peer, server);
    }
    const h2_peer_network_command_t command = {
        .type = H2_PEER_NETWORK_ADD_ICE_SERVER,
        .value.ice_server = server,
    };
    return h2_peer_network_call(peer, &command, NULL);
}

static h2_pal_result_t h2_peer_network_start_offer(h2_pal_webrtc_peer_t *peer) {
    if (!h2_peer_network_enabled(peer)) {
        return h2_peer_webrtc_start_offer(peer);
    }
    const h2_peer_network_command_t command = {
        .type = H2_PEER_NETWORK_START_OFFER,
    };
    h2_pal_result_t result = h2_peer_network_call(peer, &command, NULL);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_peer_t *owner = peer->owner;
    owner->operation_depth++;
    h2_peer_network_dispatch_available(peer, NULL);
    const int peer_closed = peer->network_cleanup_pending;
    const int finish_cleanup =
        peer_closed && peer->callback_dispatch_depth == 0u;
    owner->operation_depth--;
    if (finish_cleanup) {
        h2_peer_network_destroy_resources(peer);
    }
    h2_peer_finish_destroy(owner);
    return peer_closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_network_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                               h2_pal_webrtc_sdp_type_t type,
                               h2_pal_webrtc_str_t sdp) {
    if (!h2_peer_network_enabled(peer)) {
        return h2_peer_webrtc_set_remote_sdp(peer, type, sdp);
    }
    const h2_peer_network_command_t command = {
        .type = H2_PEER_NETWORK_SET_REMOTE_SDP,
        .value.remote_sdp =
            {
                .type = type,
                .sdp = sdp,
            },
    };
    return h2_peer_network_call(peer, &command, NULL);
}

static h2_pal_result_t h2_peer_network_create_data_channel(
    h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_channel_config_t *config,
    h2_pal_webrtc_channel_t **out_channel) {
    if (!h2_peer_network_enabled(peer)) {
        return h2_peer_webrtc_create_data_channel(peer, config, out_channel);
    }
    *out_channel = NULL;
    const h2_peer_network_command_t command = {
        .type = H2_PEER_NETWORK_CREATE_DATA_CHANNEL,
        .value.channel_config = config,
    };
    h2_peer_network_response_t response;
    h2_pal_result_t result = h2_peer_network_call(peer, &command, &response);
    if (result == H2_PAL_OK) {
        *out_channel = response.channel;
    }
    return result;
}

static h2_pal_result_t
h2_peer_network_set_media_track(h2_pal_webrtc_peer_t *peer,
                                h2_pal_webrtc_track_t *track) {
    if (!h2_peer_network_enabled(peer)) {
        return h2_peer_webrtc_set_media_track(peer, track);
    }
    const h2_peer_network_command_t command = {
        .type = H2_PEER_NETWORK_SET_MEDIA_TRACK,
        .value.media_track = track,
    };
    return h2_peer_network_call(peer, &command, NULL);
}

static h2_pal_result_t h2_peer_network_poll(h2_pal_webrtc_peer_t *peer,
                                            int timeout_ms) {
    if (!h2_peer_network_enabled(peer)) {
        h2_pal_result_t media_result = h2_peer_media_track_service(peer);
        if (media_result != H2_PAL_OK)
            return media_result;
        return h2_peer_webrtc_poll(peer, timeout_ms);
    }
    if (timeout_ms < 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_peer_t *owner = peer->owner;
    owner->operation_depth++;
    h2_peer_network_event_t *event = NULL;
    h2_pal_result_t result = (h2_pal_result_t)h2_pal_queue_recv(
        owner->config.queue, peer->network_events, &event,
        (uint32_t)timeout_ms);
    if (result == H2_PAL_OK) {
        h2_peer_network_dispatch_available(peer, event);
    } else if (result == H2_PAL_ERR_TIMEOUT) {
        result = H2_PAL_OK;
    }
    h2_pal_result_t transport_result = (h2_pal_result_t)atomic_load_explicit(
        &peer->network_transport_result, memory_order_acquire);
    if (result == H2_PAL_OK && transport_result != H2_PAL_OK &&
        atomic_load_explicit(&peer->network_event_count,
                             memory_order_acquire) == 0u) {
        result = transport_result;
    }
    const int finish_cleanup =
        peer->network_cleanup_pending && peer->callback_dispatch_depth == 0u;
    owner->operation_depth--;
    if (finish_cleanup) {
        h2_peer_network_destroy_resources(peer);
    }
    h2_peer_finish_destroy(owner);
    return result;
}

static h2_pal_result_t h2_peer_network_enqueue_opus(
    h2_pal_webrtc_peer_t *peer, const uint8_t *opus, size_t opus_len) {
    h2_peer_tx_item_t *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(
            &peer->rtp_pending, &expected, H2_PEER_TX_RESERVED,
            memory_order_acq_rel, memory_order_relaxed)) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    h2_peer_tx_item_t *item = h2_peer_prepare_tx_item(
        peer->owner, &peer->rtp_storage, opus, opus_len, 0);
    if (item == NULL) {
        atomic_store_explicit(&peer->rtp_pending, NULL, memory_order_release);
        return H2_PAL_ERR_NO_MEMORY;
    }
    atomic_store_explicit(&peer->rtp_pending, item, memory_order_release);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_peer_network_send_opus(h2_pal_webrtc_peer_t *peer,
                                                 const uint8_t *opus,
                                                 size_t opus_len) {
    if (!h2_peer_network_enabled(peer)) {
        return h2_peer_webrtc_send_opus(peer, opus, opus_len);
    }
    if (peer->closed || peer->state != H2_PAL_WEBRTC_PEER_CONNECTED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return h2_peer_network_enqueue_opus(peer, opus, opus_len);
}

static h2_pal_result_t h2_peer_media_track_service(h2_pal_webrtc_peer_t *peer) {
    h2_pal_webrtc_track_t *track = peer->media_track;
    if (track == NULL || track->config.read == NULL ||
        peer->state != H2_PAL_WEBRTC_PEER_CONNECTED || peer->closed) {
        return H2_PAL_OK;
    }
    if (track->pending_opus_len == 0u) {
        size_t opus_len = 0u;
        h2_pal_result_t result =
            track->config.read(track->config.user, track->pending_opus,
                               sizeof(track->pending_opus), &opus_len);
        if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT)
            return H2_PAL_OK;
        if (result != H2_PAL_OK)
            return result;
        if (opus_len == 0u || opus_len > sizeof(track->pending_opus))
            return H2_PAL_ERR_FORMAT;
        track->pending_opus_len = opus_len;
    }
    h2_pal_result_t result =
        h2_peer_network_enabled(peer)
            ? h2_peer_network_enqueue_opus(peer, track->pending_opus,
                                           track->pending_opus_len)
            : h2_peer_webrtc_send_opus(peer, track->pending_opus,
                                       track->pending_opus_len);
    if (result == H2_PAL_OK)
        track->pending_opus_len = 0u;
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

static h2_pal_result_t h2_peer_network_receive_opus(h2_pal_webrtc_peer_t *peer,
                                                    uint8_t *opus,
                                                    size_t opus_capacity,
                                                    size_t *out_opus_len,
                                                    uint32_t timeout_ms) {
    if (!h2_peer_network_enabled(peer)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (!h2_peer_opus_pull_enabled(peer) || peer->opus_rx_gate == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t result = h2_peer_receive_take(
        peer, peer->opus_rx_storage, peer->opus_rx_state, &peer->opus_rx_count,
        &peer->opus_rx_read_cursor, peer->opus_rx_gate, opus, opus_capacity,
        out_opus_len, NULL, timeout_ms);
    if ((result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK) &&
        peer->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    return result;
}

static h2_pal_result_t
h2_peer_network_channel_send(h2_pal_webrtc_channel_t *channel,
                             const uint8_t *data, size_t len, int is_text) {
    h2_pal_webrtc_peer_t *peer = channel->owner;
    if (!h2_peer_network_enabled(peer)) {
        return h2_peer_webrtc_channel_send(channel, data, len, is_text);
    }
    if (peer->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (!channel->open || channel->terminal ||
        peer->state != H2_PAL_WEBRTC_PEER_CONNECTED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const size_t slot = 0u;
    unsigned char expected = 0u;
    if (!atomic_compare_exchange_strong_explicit(
            &channel->tx_state[slot], &expected, 1u, memory_order_acq_rel,
            memory_order_relaxed)) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    h2_peer_tx_item_t *item = h2_peer_prepare_tx_item(
        peer->owner, &channel->tx_storage[slot], data, len, is_text);
    if (item == NULL) {
        atomic_store_explicit(&channel->tx_state[slot], 0u,
                              memory_order_release);
        return H2_PAL_ERR_NO_MEMORY;
    }
    atomic_store_explicit(&channel->tx_state[slot], 2u, memory_order_release);
    h2_peer_channel_ready_set(channel);
    return H2_PAL_OK;
}

static h2_pal_result_t
h2_peer_network_channel_receive(h2_pal_webrtc_channel_t *channel, uint8_t *data,
                                size_t capacity, size_t *out_len,
                                int *out_is_text, uint32_t timeout_ms) {
    h2_pal_webrtc_peer_t *peer = channel->owner;
    if (!h2_peer_network_enabled(peer)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (!h2_peer_channel_pull_enabled(peer) || channel->rx_gate == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t result = h2_peer_receive_take(
        peer, channel->rx_storage, channel->rx_state, &channel->rx_count,
        &channel->rx_read_cursor, channel->rx_gate, data, capacity, out_len,
        out_is_text, timeout_ms);
    if ((result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK) &&
        (peer->closed || channel->terminal)) {
        return H2_PAL_ERR_CLOSED;
    }
    return result;
}

static void h2_peer_network_channel_close(h2_pal_webrtc_channel_t *channel) {
    h2_pal_webrtc_peer_t *peer = channel->owner;
    if (!h2_peer_network_enabled(peer)) {
        h2_peer_webrtc_channel_close(channel);
        return;
    }
    const h2_peer_network_command_t command = {
        .type = H2_PEER_NETWORK_CHANNEL_CLOSE,
        .value.channel = channel,
    };
    if (h2_peer_network_call(peer, &command, NULL) != H2_PAL_OK) {
        return;
    }
    h2_peer_t *owner = peer->owner;
    owner->operation_depth++;
    h2_peer_network_dispatch_available(peer, NULL);
    const int finish_cleanup =
        peer->network_cleanup_pending && peer->callback_dispatch_depth == 0u;
    owner->operation_depth--;
    if (finish_cleanup) {
        h2_peer_network_destroy_resources(peer);
    }
    h2_peer_finish_destroy(owner);
}

static void h2_peer_network_peer_close(h2_pal_webrtc_peer_t *peer) {
    if (!h2_peer_network_enabled(peer)) {
        h2_peer_webrtc_peer_close(peer);
        return;
    }
    if (atomic_load_explicit(&peer->network_stopped, memory_order_acquire)) {
        return;
    }
    h2_peer_t *owner = peer->owner;
    owner->operation_depth++;
    const h2_peer_network_command_t command = {
        .type = H2_PEER_NETWORK_PEER_CLOSE,
    };
    if (h2_peer_network_call(peer, &command, NULL) != H2_PAL_OK) {
        owner->operation_depth--;
        h2_peer_finish_destroy(owner);
        return;
    }
    if (peer->network_task == NULL ||
        h2_pal_task_join(owner->config.task, peer->network_task) != H2_PAL_OK) {
        owner->operation_depth--;
        h2_peer_finish_destroy(owner);
        return;
    }
    atomic_store_explicit(&peer->network_stopped, 1, memory_order_release);
    h2_peer_network_dispatch_available(peer, NULL);
    peer->network_cleanup_pending = 1;
    const int finish_cleanup = peer->callback_dispatch_depth == 0u;
    owner->operation_depth--;
    if (finish_cleanup) {
        h2_peer_network_destroy_resources(peer);
    }
    h2_peer_finish_destroy(owner);
}

static const h2_pal_webrtc_vtable_t h2_peer_webrtc_vtable = {
    .peer_create = h2_peer_network_peer_create,
    .peer_create_pull = h2_peer_network_peer_create_pull,
    .peer_add_ice_server = h2_peer_network_add_ice_server,
    .peer_start_offer = h2_peer_network_start_offer,
    .peer_set_remote_sdp = h2_peer_network_set_remote_sdp,
    .peer_create_data_channel = h2_peer_network_create_data_channel,
    .peer_set_media_track = h2_peer_network_set_media_track,
    .peer_poll = h2_peer_network_poll,
    .peer_send_opus = h2_peer_network_send_opus,
    .peer_receive_opus = h2_peer_network_receive_opus,
    .channel_send = h2_peer_network_channel_send,
    .channel_receive = h2_peer_network_channel_receive,
    .channel_close = h2_peer_network_channel_close,
    .peer_close = h2_peer_network_peer_close,
};

#if !H2_PEER_PRODUCTION_ONLY
h2_pal_result_t
h2_peer_create_with_providers(const h2_peer_config_t *config,
                              const h2_peer_provider_bundle_t *providers,
                              h2_peer_t **out_peer) {
    if (out_peer != NULL) {
        *out_peer = NULL;
    }
    if (out_peer == NULL || !h2_peer_config_valid(config) ||
        !h2_peer_providers_valid(providers)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_peer_t bootstrap = {.config = *config};
    h2_peer_t *peer =
        (h2_peer_t *)h2_peer_control_alloc(&bootstrap, sizeof(*peer));
    if (peer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    atomic_init(&peer->operation_depth, 0u);
    peer->config = *config;
    peer->providers = *providers;
    peer->webrtc_api.user = peer;
    peer->webrtc_api.vtable = &h2_peer_webrtc_vtable;
    *out_peer = peer;
    return H2_PAL_OK;
}
#endif

h2_pal_result_t h2_peer_create(const h2_peer_config_t *config,
                               h2_peer_t **out_peer) {
    if (out_peer != NULL) {
        *out_peer = NULL;
    }
    if (out_peer == NULL || !h2_peer_config_valid(config)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_peer_production_config_supported(config)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
#if H2_PEER_PRODUCTION_ONLY
    h2_peer_t bootstrap = {.config = *config};
    h2_peer_t *peer =
        (h2_peer_t *)h2_peer_control_alloc(&bootstrap, sizeof(*peer));
    if (peer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    atomic_init(&peer->operation_depth, 0u);
    peer->config = *config;
    peer->webrtc_api.user = peer;
    peer->webrtc_api.vtable = &h2_peer_webrtc_vtable;
    peer->production_backend = 1;
    *out_peer = peer;
    return H2_PAL_OK;
#else
    h2_pal_result_t result = h2_peer_create_with_providers(
        config, h2_peer_unavailable_providers(), out_peer);
    if (result == H2_PAL_OK) {
        (*out_peer)->production_backend = 1;
    }
    return result;
#endif
}

const h2_pal_webrtc_api_t *h2_peer_webrtc_api(h2_peer_t *peer) {
    return peer == NULL || peer->destroying ? NULL : &peer->webrtc_api;
}

h2_pal_result_t
h2_peer_media_track_create(h2_peer_t *peer,
                           const h2_peer_media_track_config_t *config,
                           h2_pal_webrtc_track_t **out_track) {
    if (peer == NULL || peer->destroying || config == NULL ||
        (config->read == NULL && config->write == NULL) || out_track == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_track = NULL;
    h2_pal_webrtc_track_t *track =
        (h2_pal_webrtc_track_t *)h2_peer_alloc(peer, sizeof(*track));
    if (track == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    track->owner = peer;
    track->config = *config;
    track->next = peer->tracks;
    peer->tracks = track;
    *out_track = track;
    return H2_PAL_OK;
}

h2_pal_result_t h2_peer_media_track_destroy(h2_pal_webrtc_track_t **track_ptr) {
    if (track_ptr == NULL || *track_ptr == NULL)
        return H2_PAL_OK;
    h2_pal_webrtc_track_t *track = *track_ptr;
    if (track->owner == NULL || track->owner->destroying)
        return H2_PAL_ERR_CLOSED;
    if (track->bound_peer != NULL)
        return H2_PAL_ERR_INVALID_STATE;
    h2_peer_t *owner = track->owner;
    h2_pal_webrtc_track_t **cursor = &owner->tracks;
    while (*cursor != NULL && *cursor != track)
        cursor = &(*cursor)->next;
    if (*cursor != track)
        return H2_PAL_ERR_INVALID_ARG;
    *cursor = track->next;
    *track_ptr = NULL;
    h2_peer_free(owner, track);
    return H2_PAL_OK;
}

void h2_peer_destroy(h2_peer_t **peer) {
    if (peer == NULL || *peer == NULL) {
        return;
    }
    h2_peer_t *owner = *peer;
    *peer = NULL;
    if (owner->destroying) {
        return;
    }
    owner->destroying = 1;
    if (owner->production_backend) {
        owner->operation_depth++;
        while (owner->peers != NULL) {
            h2_peer_network_peer_close(owner->peers);
        }
        owner->operation_depth--;
        h2_peer_finish_destroy(owner);
        return;
    }
    for (h2_pal_webrtc_peer_t *current = owner->peers; current != NULL;
         current = current->next) {
        current->closed = 1;
        current->close_pending = 1;
    }
    h2_peer_finish_destroy(owner);
}

#if !H2_PEER_PRODUCTION_ONLY
h2_pal_result_t h2_peer_receive_rtp_for_test(h2_pal_webrtc_peer_t *peer,
                                             const uint8_t *packet,
                                             size_t packet_len) {
    if (peer == NULL || packet == NULL || peer->closed || !peer->srtp_open) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint8_t clear[H2_PEER_WIRE_PACKET_MAX];
    size_t clear_len = 0u;
    h2_pal_result_t result = peer->owner->providers.srtp.vtable->receive_rtp(
        peer->owner->providers.srtp.user, peer->srtp_session, packet,
        packet_len, clear, sizeof(clear), &clear_len);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_peer_rtp_packet_t parsed;
    result = h2_peer_rtp_parse(clear, clear_len, &parsed);
    if (result != H2_PAL_OK ||
        parsed.payload_type != H2_PEER_RTP_OPUS_PAYLOAD_TYPE ||
        parsed.payload_len > H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE) {
        return result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result;
    }
    h2_peer_webrtc_emit_opus_frame(peer, parsed.payload, parsed.payload_len);
    return H2_PAL_OK;
}
#endif
