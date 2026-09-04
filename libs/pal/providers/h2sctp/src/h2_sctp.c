#include "h2_sctp.h"

#include "h2_sctp_association.h"
#include "h2_sctp_internal.h"
#include "h2_sctp_reliability.h"
#include "h2_sctp_stream.h"
#include "h2_sctp_timer.h"
#include "h2_sctp_wire.h"

#include <limits.h>
#include <string.h>

void *h2_sctp_alloc(h2_sctp_t *provider, size_t size) {
    if (provider == NULL || provider->mem == NULL || size == 0u) {
        return NULL;
    }
    void *pointer = h2_pal_mem_alloc(provider->mem, size);
    if (pointer != NULL) {
        memset(pointer, 0, size);
    }
    return pointer;
}

void h2_sctp_free(h2_sctp_t *provider, void *pointer) {
    if (provider != NULL && pointer != NULL) {
        h2_pal_mem_free(provider->mem, pointer);
    }
}

uint64_t h2_sctp_deadline_add(uint64_t now_ms, uint64_t delta_ms) {
    if (delta_ms > UINT64_MAX - 1u - now_ms) {
        return UINT64_MAX - 1u;
    }
    return now_ms + delta_ms;
}

bool h2_sctp_tsn_before(uint32_t left, uint32_t right) {
    const uint32_t forward_distance = right - left;
    return forward_distance != 0u && forward_distance < 0x80000000u;
}

bool h2_sctp_tsn_after(uint32_t left, uint32_t right) {
    return h2_sctp_tsn_before(right, left);
}

h2_pal_result_t h2_sctp_validate_operation(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    if (association == NULL || now_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (association->in_callback) {
        return H2_PAL_ERR_BUSY;
    }
    if (association->time_initialized && now_ms < association->last_now_ms) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    association->time_initialized = true;
    association->last_now_ms = now_ms;
    return H2_PAL_OK;
}

void h2_sctp_notify_state(
    h2_pal_sctp_association_t *association,
    h2_pal_sctp_state_t state,
    h2_pal_result_t reason) {
    if (association == NULL || association->state == state) {
        return;
    }
    association->state = state;
    association->terminal_reason = reason;
    association->in_callback = true;
    association->config.callbacks.on_state(
        association->config.callbacks.user, association, state, reason);
    association->in_callback = false;
}

h2_pal_result_t h2_sctp_notify_message(
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_received_message_t *message) {
    association->in_callback = true;
    h2_pal_result_t result = association->config.callbacks.on_message(
        association->config.callbacks.user, association, message);
    association->in_callback = false;
    return result;
}

void h2_sctp_notify_stream_reset(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    h2_pal_sctp_stream_reset_direction_t direction,
    h2_pal_result_t result) {
    const h2_pal_sctp_stream_reset_event_t event = {
        .stream_id = stream_id,
        .direction = direction,
        .result = result,
    };
    association->in_callback = true;
    association->config.callbacks.on_stream_reset(
        association->config.callbacks.user, association, &event);
    association->in_callback = false;
}

void h2_sctp_clear_control(h2_pal_sctp_association_t *association) {
    if (association == NULL) {
        return;
    }
    h2_sctp_free(association->owner, association->control_packet);
    association->control_packet = NULL;
    association->control_packet_len = 0u;
    association->control_kind = H2_SCTP_CONTROL_NONE;
    association->control_retries = 0u;
    association->control_reset_in_progress = false;
    association->control_reset_retries = 0u;
    association->control_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
}

void h2_sctp_fail(
    h2_pal_sctp_association_t *association,
    h2_pal_result_t reason) {
    if (association == NULL || association->state == H2_PAL_SCTP_STATE_FAILED ||
        association->state == H2_PAL_SCTP_STATE_CLOSED) {
        return;
    }
    if (reason == H2_PAL_OK) {
        reason = H2_PAL_ERR_IO;
    }
    h2_sctp_clear_control(association);
    h2_sctp_notify_state(association, H2_PAL_SCTP_STATE_FAILED, reason);
}

static h2_pal_result_t h2_sctp_invoke_emit(
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len) {
    association->in_callback = true;
    const h2_pal_result_t result = association->config.callbacks.emit_packet(
        association->config.callbacks.user,
        association,
        packet,
        packet_len);
    association->in_callback = false;
    return result;
}

h2_pal_result_t h2_sctp_emit_chunks(
    h2_pal_sctp_association_t *association,
    uint32_t verification_tag,
    const uint8_t *chunks,
    size_t chunks_len,
    h2_sctp_control_kind_t control_kind,
    uint64_t now_ms) {
    if (association->pending_emit != NULL) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    uint8_t *packet = h2_sctp_alloc(
        association->owner, association->config.max_packet_size);
    if (packet == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    size_t packet_len = 0u;
    h2_pal_result_t result = h2_sctp_wire_build_packet(
        association->config.local_port,
        association->config.remote_port,
        verification_tag,
        chunks,
        chunks_len,
        packet,
        association->config.max_packet_size,
        &packet_len);
    if (result != H2_PAL_OK) {
        h2_sctp_free(association->owner, packet);
        return result;
    }

    if (control_kind != H2_SCTP_CONTROL_NONE) {
        uint8_t *control = h2_sctp_alloc(association->owner, packet_len);
        if (control == NULL) {
            h2_sctp_free(association->owner, packet);
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(control, packet, packet_len);
        h2_sctp_clear_control(association);
        association->control_packet = control;
        association->control_packet_len = packet_len;
        association->control_kind = control_kind;
        association->control_deadline_ms = h2_sctp_deadline_add(
            now_ms, association->rto_ms);
    }

    result = h2_sctp_invoke_emit(association, packet, packet_len);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        association->pending_emit = packet;
        association->pending_emit_len = packet_len;
        return result;
    }
    h2_sctp_free(association->owner, packet);
    if (result != H2_PAL_OK) {
        h2_sctp_fail(association, result);
    }
    return result;
}

h2_pal_result_t h2_sctp_retry_pending_emit(
    h2_pal_sctp_association_t *association) {
    if (association->pending_emit == NULL) {
        return H2_PAL_OK;
    }
    const h2_pal_result_t result = h2_sctp_invoke_emit(
        association, association->pending_emit, association->pending_emit_len);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    h2_sctp_free(association->owner, association->pending_emit);
    association->pending_emit = NULL;
    association->pending_emit_len = 0u;
    if (result != H2_PAL_OK) {
        h2_sctp_fail(association, result);
    }
    return result;
}

static h2_pal_result_t h2_sctp_vtable_association_create(
    void *user,
    const h2_pal_sctp_association_config_t *config,
    h2_pal_sctp_association_t **out_association) {
    h2_sctp_t *provider = (h2_sctp_t *)user;
    uint8_t random[8] = {0};
    h2_pal_sctp_association_t *association = h2_sctp_alloc(
        provider, sizeof(*association));
    if (association == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const h2_pal_result_t random_result = (h2_pal_result_t)h2_pal_crypto_random(
        provider->crypto, random, sizeof(random));
    if (random_result != H2_PAL_OK) {
        h2_sctp_free(provider, association);
        return random_result;
    }
    association->owner = provider;
    association->config = *config;
    association->state = H2_PAL_SCTP_STATE_NEW;
    association->terminal_reason = H2_PAL_OK;
    association->local_verification_tag = h2_sctp_wire_read_u32(random);
    if (association->local_verification_tag == 0u) {
        association->local_verification_tag = 1u;
    }
    association->initial_tsn = h2_sctp_wire_read_u32(random + 4u);
    association->next_tsn = association->initial_tsn;
    association->peer_cumulative_tsn = association->initial_tsn - 1u;
    association->advanced_peer_ack = association->peer_cumulative_tsn;
    association->next_reset_sequence = association->initial_tsn;
    association->expected_reset_sequence = 0u;
    association->rto_ms = H2_SCTP_RTO_INITIAL_MS;
    const size_t initial_cwnd =
        config->max_packet_size > SIZE_MAX / H2_SCTP_INITIAL_CWND_PACKETS
            ? SIZE_MAX
            : config->max_packet_size * H2_SCTP_INITIAL_CWND_PACKETS;
    association->cwnd = initial_cwnd > UINT32_MAX
                            ? UINT32_MAX
                            : (uint32_t)initial_cwnd;
    association->ssthresh = UINT32_MAX;
    association->peer_receive_window = UINT32_MAX;
    association->rx_fragments_tail = NULL;
    association->control_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
    association->heartbeat_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
    association->sack_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
    association->next = provider->associations;
    provider->associations = association;
    provider->association_count++;
    *out_association = association;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_sctp_vtable_association_start(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    if (user != association->owner) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_sctp_association_start_impl(association, now_ms);
}

static h2_pal_result_t h2_sctp_vtable_association_input_packet(
    void *user,
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len,
    uint64_t now_ms) {
    if (user != association->owner) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_sctp_association_input_impl(
        association, packet, packet_len, now_ms);
}

static h2_pal_result_t h2_sctp_vtable_association_service(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *out_next_deadline_ms) {
    if (user != association->owner) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_sctp_timer_service(
        association, now_ms, out_next_deadline_ms);
}

static h2_pal_result_t h2_sctp_vtable_association_send_message(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_message_t *message,
    uint64_t now_ms) {
    if (user != association->owner) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = h2_sctp_validate_operation(association, now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (association->state != H2_PAL_SCTP_STATE_CONNECTED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (message->stream_id >= association->negotiated_outbound_streams ||
        message->len > association->config.max_message_size) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!association->peer_interleaving && message->len > 16u * 1024u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    result = h2_sctp_stream_queue_message(association, message, now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = h2_sctp_reliability_send_pending(association, now_ms);
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

static h2_pal_result_t h2_sctp_vtable_association_is_writable(
    void *user,
    h2_pal_sctp_association_t *association,
    bool *out_writable) {
    if (user != association->owner) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint32_t send_limit = association->cwnd;
    if (association->peer_receive_window < send_limit) {
        send_limit = association->peer_receive_window;
    }
    *out_writable =
        association->state == H2_PAL_SCTP_STATE_CONNECTED &&
        association->pending_emit == NULL &&
        association->send_used < association->config.send_buffer_size &&
        association->flight_size < send_limit;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_sctp_vtable_association_reset_stream(
    void *user,
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    uint64_t now_ms) {
    if (user != association->owner) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_sctp_association_reset_stream_impl(
        association, stream_id, now_ms);
}

static h2_pal_result_t h2_sctp_vtable_association_shutdown(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    if (user != association->owner) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_sctp_association_shutdown_impl(association, now_ms);
}

static h2_pal_result_t h2_sctp_vtable_association_abort(
    void *user,
    h2_pal_sctp_association_t *association,
    h2_pal_result_t reason,
    uint64_t now_ms) {
    if (user != association->owner) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_sctp_association_abort_impl(association, reason, now_ms);
}

static h2_pal_result_t h2_sctp_vtable_association_close(
    void *user,
    h2_pal_sctp_association_t **association_pointer) {
    h2_sctp_t *provider = (h2_sctp_t *)user;
    h2_pal_sctp_association_t *association = *association_pointer;
    if (association->owner != provider) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (association->in_callback) {
        return H2_PAL_ERR_BUSY;
    }
    h2_pal_sctp_association_t **cursor = &provider->associations;
    while (*cursor != NULL && *cursor != association) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *cursor = association->next;
    provider->association_count--;
    h2_sctp_clear_control(association);
    h2_sctp_free(provider, association->pending_emit);
    h2_sctp_free(provider, association->peer_cookie);
    h2_sctp_reliability_release_all(association);
    h2_sctp_stream_release_all(association);
    h2_sctp_free(provider, association);
    *association_pointer = NULL;
    return H2_PAL_OK;
}

static const h2_pal_sctp_vtable_t h2_sctp_vtable = {
    .association_create = h2_sctp_vtable_association_create,
    .association_start = h2_sctp_vtable_association_start,
    .association_input_packet = h2_sctp_vtable_association_input_packet,
    .association_service = h2_sctp_vtable_association_service,
    .association_send_message = h2_sctp_vtable_association_send_message,
    .association_is_writable = h2_sctp_vtable_association_is_writable,
    .association_reset_stream = h2_sctp_vtable_association_reset_stream,
    .association_shutdown = h2_sctp_vtable_association_shutdown,
    .association_abort = h2_sctp_vtable_association_abort,
    .association_close = h2_sctp_vtable_association_close,
};

h2_pal_result_t h2_sctp_create(
    const h2_sctp_config_t *config,
    h2_sctp_t **out_provider) {
    if (out_provider == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_provider = NULL;
    if (config == NULL || config->mem == NULL ||
        config->mem->vtable == NULL || config->mem->vtable->alloc == NULL ||
        config->mem->vtable->free == NULL || config->crypto == NULL ||
        config->crypto->vtable == NULL ||
        config->crypto->vtable->random == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_sctp_t temporary = {
        .mem = config->mem,
        .crypto = config->crypto,
    };
    h2_sctp_t *provider = h2_sctp_alloc(&temporary, sizeof(*provider));
    if (provider == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    provider->mem = config->mem;
    provider->crypto = config->crypto;
    provider->api.user = provider;
    provider->api.vtable = &h2_sctp_vtable;
    *out_provider = provider;
    return H2_PAL_OK;
}

const h2_pal_sctp_api_t *h2_sctp_api(h2_sctp_t *provider) {
    return provider == NULL ? NULL : &provider->api;
}

h2_pal_result_t h2_sctp_destroy(h2_sctp_t **provider_pointer) {
    if (provider_pointer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (*provider_pointer == NULL) {
        return H2_PAL_OK;
    }
    h2_sctp_t *provider = *provider_pointer;
    if (provider->association_count != 0u || provider->associations != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const h2_pal_mem_api_t *mem = provider->mem;
    h2_pal_mem_free(mem, provider);
    *provider_pointer = NULL;
    return H2_PAL_OK;
}
