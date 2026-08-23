#ifndef H2_PAL_SCTP_H
#define H2_PAL_SCTP_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Smallest packet budget accepted by an SCTP association. */
#define H2_PAL_SCTP_MIN_PACKET_SIZE 128u
/** Sentinel returned when an association has no scheduled work. */
#define H2_PAL_SCTP_NO_DEADLINE UINT64_MAX

/** Provider-owned mutable association handle. */
typedef struct h2_pal_sctp_association h2_pal_sctp_association_t;

/** Establishment role selected explicitly by the caller. */
typedef enum h2_pal_sctp_role {
    /** Sends INIT when started. */
    H2_PAL_SCTP_ROLE_ACTIVE = 0,
    /** Waits for INIT when started. */
    H2_PAL_SCTP_ROLE_PASSIVE = 1,
} h2_pal_sctp_role_t;

/** Observable association lifecycle state. */
typedef enum h2_pal_sctp_state {
    H2_PAL_SCTP_STATE_NEW = 0,
    H2_PAL_SCTP_STATE_CONNECTING = 1,
    H2_PAL_SCTP_STATE_CONNECTED = 2,
    H2_PAL_SCTP_STATE_SHUTTING_DOWN = 3,
    H2_PAL_SCTP_STATE_CLOSED = 4,
    H2_PAL_SCTP_STATE_FAILED = 5,
} h2_pal_sctp_state_t;

/** Delivery policy attached to one outbound message. */
typedef enum h2_pal_sctp_reliability {
    H2_PAL_SCTP_RELIABILITY_RELIABLE = 0,
    H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS = 1,
    H2_PAL_SCTP_RELIABILITY_MAX_LIFETIME_MS = 2,
} h2_pal_sctp_reliability_t;

/** Direction described by a stream-reset callback. */
typedef enum h2_pal_sctp_stream_reset_direction {
    H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED = 0,
    H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET = 1,
} h2_pal_sctp_stream_reset_direction_t;

/** Complete outbound user message copied by a successful submission. */
typedef struct h2_pal_sctp_message {
    /** Borrowed payload valid for the synchronous call. */
    const uint8_t *data;
    /** Nonzero payload length. */
    size_t len;
    /** Negotiated outbound stream identifier. */
    uint16_t stream_id;
    /** Opaque application payload protocol identifier. */
    uint32_t ppid;
    /** True for unordered delivery. */
    bool unordered;
    /** Reliability policy. */
    h2_pal_sctp_reliability_t reliability;
    /** Retransmit count or lifetime in milliseconds, according to policy. */
    uint32_t reliability_value;
} h2_pal_sctp_message_t;

/** Complete inbound user message borrowed for one callback. */
typedef struct h2_pal_sctp_received_message {
    const uint8_t *data;
    size_t len;
    uint16_t stream_id;
    uint32_t ppid;
    bool unordered;
} h2_pal_sctp_received_message_t;

/** Completion evidence for one local or peer stream reset. */
typedef struct h2_pal_sctp_stream_reset_event {
    uint16_t stream_id;
    h2_pal_sctp_stream_reset_direction_t direction;
    h2_pal_result_t result;
} h2_pal_sctp_stream_reset_event_t;

/**
 * Emits one complete SCTP packet to a caller-owned secure transport.
 *
 * OK consumes the packet. WOULD_BLOCK consumes none; the provider retains the
 * exact packet and retries it from service before emitting later packets.
 * Other errors are fatal to the association. The packet is borrowed only for
 * this synchronous callback.
 */
typedef h2_pal_result_t (*h2_pal_sctp_emit_packet_fn)(
    void *user,
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len);

/** Reports each actual association state transition at most once. */
typedef void (*h2_pal_sctp_state_fn)(
    void *user,
    h2_pal_sctp_association_t *association,
    h2_pal_sctp_state_t state,
    h2_pal_result_t reason);

/**
 * Delivers one complete reassembled message as a borrowed callback view.
 *
 * OK consumes the message. WOULD_BLOCK retains it in the association receive
 * window and retries delivery from a later service call. Other errors are
 * fatal to the association.
 */
typedef h2_pal_result_t (*h2_pal_sctp_message_fn)(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_received_message_t *message);

/**
 * Reports local reset completion or an incoming peer reset at most once for
 * each accepted RFC 6525 request or response. Wire retransmissions must not
 * produce duplicate callbacks, and a successful callback must not be repeated
 * after the same numeric stream ID is reused.
 */
typedef void (*h2_pal_sctp_stream_reset_fn)(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_stream_reset_event_t *event);

/** Required synchronous callbacks copied into an association. */
typedef struct h2_pal_sctp_callbacks {
    void *user;
    h2_pal_sctp_emit_packet_fn emit_packet;
    h2_pal_sctp_state_fn on_state;
    h2_pal_sctp_message_fn on_message;
    h2_pal_sctp_stream_reset_fn on_stream_reset;
} h2_pal_sctp_callbacks_t;

/** Explicit association limits, role, ports, and callback ownership. */
typedef struct h2_pal_sctp_association_config {
    /** Active or passive establishment role. */
    h2_pal_sctp_role_t role;
    /** Nonzero local SCTP port. */
    uint16_t local_port;
    /** Nonzero remote SCTP port. */
    uint16_t remote_port;
    /** Nonzero requested inbound stream count. */
    uint16_t inbound_streams;
    /** Nonzero requested outbound stream count. */
    uint16_t outbound_streams;
    /** Complete packet budget in H2_PAL_SCTP_MIN_PACKET_SIZE..UINT16_MAX. */
    size_t max_packet_size;
    /** Maximum complete user message accepted or reassembled. */
    size_t max_message_size;
    /** Bounded storage for copied outbound messages and packet retention. */
    size_t send_buffer_size;
    /** Bounded storage for out-of-order and reassembly data. */
    size_t receive_buffer_size;
    /** Lifetime of a passive stateful cookie in monotonic milliseconds. */
    uint64_t cookie_lifetime_ms;
    /** Required callbacks and caller-owned context. */
    h2_pal_sctp_callbacks_t callbacks;
} h2_pal_sctp_association_config_t;

/** Provider operations; consumers call the checked wrappers below. */
typedef struct h2_pal_sctp_vtable {
    h2_pal_result_t (*association_create)(
        void *user,
        const h2_pal_sctp_association_config_t *config,
        h2_pal_sctp_association_t **out_association);
    h2_pal_result_t (*association_start)(
        void *user,
        h2_pal_sctp_association_t *association,
        uint64_t now_ms);
    h2_pal_result_t (*association_input_packet)(
        void *user,
        h2_pal_sctp_association_t *association,
        const uint8_t *packet,
        size_t packet_len,
        uint64_t now_ms);
    h2_pal_result_t (*association_service)(
        void *user,
        h2_pal_sctp_association_t *association,
        uint64_t now_ms,
        uint64_t *out_next_deadline_ms);
    h2_pal_result_t (*association_send_message)(
        void *user,
        h2_pal_sctp_association_t *association,
        const h2_pal_sctp_message_t *message,
        uint64_t now_ms);
    h2_pal_result_t (*association_is_writable)(
        void *user,
        h2_pal_sctp_association_t *association,
        bool *out_writable);
    h2_pal_result_t (*association_reset_stream)(
        void *user,
        h2_pal_sctp_association_t *association,
        uint16_t stream_id,
        uint64_t now_ms);
    h2_pal_result_t (*association_shutdown)(
        void *user,
        h2_pal_sctp_association_t *association,
        uint64_t now_ms);
    h2_pal_result_t (*association_abort)(
        void *user,
        h2_pal_sctp_association_t *association,
        h2_pal_result_t reason,
        uint64_t now_ms);
    h2_pal_result_t (*association_close)(
        void *user,
        h2_pal_sctp_association_t **association);
} h2_pal_sctp_vtable_t;

/** Provider-neutral SCTP capability object. */
typedef struct h2_pal_sctp_api {
    void *user;
    const h2_pal_sctp_vtable_t *vtable;
} h2_pal_sctp_api_t;

static inline bool h2_pal_sctp_role_is_valid(h2_pal_sctp_role_t role) {
    return role == H2_PAL_SCTP_ROLE_ACTIVE ||
           role == H2_PAL_SCTP_ROLE_PASSIVE;
}

static inline bool h2_pal_sctp_reliability_is_valid(
    h2_pal_sctp_reliability_t reliability,
    uint32_t value) {
    if (reliability == H2_PAL_SCTP_RELIABILITY_RELIABLE) {
        return value == 0u;
    }
    if (reliability == H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS) {
        return true;
    }
    return reliability == H2_PAL_SCTP_RELIABILITY_MAX_LIFETIME_MS;
}

static inline bool h2_pal_sctp_config_is_valid(
    const h2_pal_sctp_association_config_t *config) {
    return config != NULL && h2_pal_sctp_role_is_valid(config->role) &&
           config->local_port != 0u && config->remote_port != 0u &&
           config->inbound_streams != 0u && config->outbound_streams != 0u &&
           config->max_packet_size >= H2_PAL_SCTP_MIN_PACKET_SIZE &&
           config->max_packet_size <= UINT16_MAX &&
           config->max_message_size != 0u &&
           config->receive_buffer_size >= 1500u &&
           config->receive_buffer_size >= config->max_message_size &&
           config->send_buffer_size >= config->max_message_size &&
           config->send_buffer_size - config->max_message_size >=
               config->max_packet_size &&
           config->cookie_lifetime_ms != 0u &&
           config->callbacks.emit_packet != NULL &&
           config->callbacks.on_state != NULL &&
           config->callbacks.on_message != NULL &&
           config->callbacks.on_stream_reset != NULL;
}

/** Creates a NEW association and clears out_association on failure. */
static inline h2_pal_result_t h2_pal_sctp_association_create(
    const h2_pal_sctp_api_t *api,
    const h2_pal_sctp_association_config_t *config,
    h2_pal_sctp_association_t **out_association) {
    if (out_association == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_association = NULL;
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_create == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (!h2_pal_sctp_config_is_valid(config)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->association_create(
        api->user, config, out_association);
}

/** Starts active INIT emission or passive INIT waiting at absolute now_ms. */
static inline h2_pal_result_t h2_pal_sctp_association_start(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    if (association == NULL || now_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_start == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_start(api->user, association, now_ms);
}

/**
 * Offers one complete SCTP packet for synchronous consumption.
 *
 * WOULD_BLOCK is the only result that leaves the packet unconsumed for retry.
 * Packet storage is borrowed only for this call.
 */
static inline h2_pal_result_t h2_pal_sctp_association_input_packet(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len,
    uint64_t now_ms) {
    if (association == NULL || packet == NULL ||
        packet_len < 12u || now_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_input_packet == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_input_packet(
        api->user, association, packet, packet_len, now_ms);
}

/** Advances due work and returns the earliest absolute monotonic deadline. */
static inline h2_pal_result_t h2_pal_sctp_association_service(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *out_next_deadline_ms) {
    if (out_next_deadline_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_next_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
    if (association == NULL || now_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_service == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_service(
        api->user, association, now_ms, out_next_deadline_ms);
}

/** Copies and submits one complete nonempty user message. */
static inline h2_pal_result_t h2_pal_sctp_association_send_message(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_message_t *message,
    uint64_t now_ms) {
    if (association == NULL || message == NULL || message->data == NULL ||
        message->len == 0u ||
        !h2_pal_sctp_reliability_is_valid(
            message->reliability, message->reliability_value) ||
        now_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_send_message == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_send_message(
        api->user, association, message, now_ms);
}

/** Reports whether the association can currently advance outbound data. */
static inline h2_pal_result_t h2_pal_sctp_association_is_writable(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t *association,
    bool *out_writable) {
    if (out_writable == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_writable = false;
    if (association == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_is_writable == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_is_writable(
        api->user, association, out_writable);
}

/** Requests RFC 6525 reset of one negotiated outbound stream. */
static inline h2_pal_result_t h2_pal_sctp_association_reset_stream(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    uint64_t now_ms) {
    if (association == NULL || now_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_reset_stream == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_reset_stream(
        api->user, association, stream_id, now_ms);
}

/** Starts graceful shutdown after already accepted outbound data drains. */
static inline h2_pal_result_t h2_pal_sctp_association_shutdown(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    if (association == NULL || now_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_shutdown == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_shutdown(api->user, association, now_ms);
}

/** Sends non-graceful termination with a non-OK stable reason. */
static inline h2_pal_result_t h2_pal_sctp_association_abort(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t *association,
    h2_pal_result_t reason,
    uint64_t now_ms) {
    if (association == NULL || reason == H2_PAL_OK ||
        now_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_abort == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_abort(
        api->user, association, reason, now_ms);
}

/**
 * Releases local ownership immediately and clears the caller pointer.
 *
 * Closing an already-NULL association is a successful no-op and never invokes
 * a callback.
 */
static inline h2_pal_result_t h2_pal_sctp_association_close(
    const h2_pal_sctp_api_t *api,
    h2_pal_sctp_association_t **association) {
    if (association == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (*association == NULL) {
        return H2_PAL_OK;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->association_close == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->association_close(api->user, association);
}

#ifdef __cplusplus
}
#endif

#endif
