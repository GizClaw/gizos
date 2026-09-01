#include "h2_sctp_association.h"

#include "h2_sctp_reliability.h"
#include "h2_sctp_stream.h"
#include "h2_sctp_wire.h"

#include <string.h>

typedef struct h2_sctp_init_values {
    uint32_t initiate_tag;
    uint32_t initial_tsn;
    uint32_t receive_window;
    uint16_t outbound_streams;
    uint16_t inbound_streams;
    bool forward_tsn;
    bool interleaving;
    bool stream_reset;
    const uint8_t *cookie;
    size_t cookie_len;
} h2_sctp_init_values_t;

typedef struct h2_sctp_peer_state {
    uint32_t verification_tag;
    uint32_t initial_tsn;
    uint32_t cumulative_received_tsn;
    uint32_t expected_reset_sequence;
    uint32_t receive_window;
    uint16_t inbound_streams;
    uint16_t outbound_streams;
    bool forward_tsn;
    bool interleaving;
    bool stream_reset;
} h2_sctp_peer_state_t;

static uint16_t h2_sctp_min_u16(uint16_t left, uint16_t right) {
    return left < right ? left : right;
}

static h2_pal_result_t h2_sctp_association_random(
    h2_pal_sctp_association_t *association,
    uint8_t *out,
    size_t len) {
    return (h2_pal_result_t)h2_pal_crypto_random(
        association->owner->crypto, out, len);
}

static size_t h2_sctp_association_write_extensions(
    uint8_t *out,
    size_t offset) {
    h2_sctp_wire_write_u16(
        out + offset, H2_SCTP_PARAM_FORWARD_TSN_SUPPORTED);
    h2_sctp_wire_write_u16(out + offset + 2u, 4u);
    offset += 4u;

    h2_sctp_wire_write_u16(
        out + offset, H2_SCTP_PARAM_SUPPORTED_EXTENSIONS);
    h2_sctp_wire_write_u16(out + offset + 2u, 7u);
    out[offset + 4u] = H2_SCTP_CHUNK_I_DATA;
    out[offset + 5u] = H2_SCTP_CHUNK_I_FORWARD_TSN;
    out[offset + 6u] = H2_SCTP_CHUNK_RE_CONFIG;
    out[offset + 7u] = 0u;
    return offset + 8u;
}

static h2_pal_result_t h2_sctp_association_send_init(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    uint8_t chunk[32] = {0};
    chunk[0] = H2_SCTP_CHUNK_INIT;
    h2_sctp_wire_write_u32(chunk + 4u, association->local_verification_tag);
    const size_t receive_window = association->config.receive_buffer_size;
    h2_sctp_wire_write_u32(
        chunk + 8u,
        receive_window > UINT32_MAX ? UINT32_MAX : (uint32_t)receive_window);
    h2_sctp_wire_write_u16(
        chunk + 12u, association->config.outbound_streams);
    h2_sctp_wire_write_u16(
        chunk + 14u, association->config.inbound_streams);
    h2_sctp_wire_write_u32(chunk + 16u, association->initial_tsn);
    const size_t chunk_len = h2_sctp_association_write_extensions(chunk, 20u);
    h2_sctp_wire_write_u16(chunk + 2u, (uint16_t)chunk_len);
    return h2_sctp_emit_chunks(
        association,
        0u,
        chunk,
        chunk_len,
        H2_SCTP_CONTROL_INIT,
        now_ms);
}

static h2_pal_result_t h2_sctp_association_parse_init(
    const h2_sctp_chunk_view_t *chunk,
    h2_sctp_init_values_t *out_values) {
    *out_values = (h2_sctp_init_values_t){0};
    if (chunk->len < 16u) {
        return H2_PAL_ERR_TRUNCATED;
    }
    out_values->initiate_tag = h2_sctp_wire_read_u32(chunk->data);
    out_values->receive_window = h2_sctp_wire_read_u32(chunk->data + 4u);
    out_values->outbound_streams = h2_sctp_wire_read_u16(chunk->data + 8u);
    out_values->inbound_streams = h2_sctp_wire_read_u16(chunk->data + 10u);
    out_values->initial_tsn = h2_sctp_wire_read_u32(chunk->data + 12u);
    if (out_values->initiate_tag == 0u ||
        out_values->outbound_streams == 0u ||
        out_values->inbound_streams == 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    bool supports_i_data = false;
    bool supports_i_forward = false;
    size_t offset = 16u;
    while (offset < chunk->len) {
        if (chunk->len - offset < 4u) {
            return H2_PAL_ERR_TRUNCATED;
        }
        const uint16_t type = h2_sctp_wire_read_u16(chunk->data + offset);
        const size_t length = h2_sctp_wire_read_u16(chunk->data + offset + 2u);
        if (length < 4u || length > chunk->len - offset) {
            return H2_PAL_ERR_FORMAT;
        }
        if (type == H2_SCTP_PARAM_FORWARD_TSN_SUPPORTED) {
            if (length != 4u) {
                return H2_PAL_ERR_FORMAT;
            }
            out_values->forward_tsn = true;
        } else if (type == H2_SCTP_PARAM_SUPPORTED_EXTENSIONS) {
            for (size_t index = 4u; index < length; ++index) {
                supports_i_data = supports_i_data ||
                                  chunk->data[offset + index] ==
                                      H2_SCTP_CHUNK_I_DATA;
                supports_i_forward = supports_i_forward ||
                                     chunk->data[offset + index] ==
                                         H2_SCTP_CHUNK_I_FORWARD_TSN;
                out_values->stream_reset = out_values->stream_reset ||
                                           chunk->data[offset + index] ==
                                               H2_SCTP_CHUNK_RE_CONFIG;
            }
        } else if (type == H2_SCTP_PARAM_STATE_COOKIE) {
            if (length <= 4u) {
                return H2_PAL_ERR_FORMAT;
            }
            out_values->cookie = chunk->data + offset + 4u;
            out_values->cookie_len = length - 4u;
        }
        if (length > SIZE_MAX - 3u) {
            return H2_PAL_ERR_FORMAT;
        }
        const size_t padded = (length + 3u) & ~(size_t)3u;
        if (padded > chunk->len - offset) {
            if (length != chunk->len - offset) {
                return H2_PAL_ERR_TRUNCATED;
            }
            offset += length;
            continue;
        }
        offset += padded;
    }
    out_values->interleaving = supports_i_data && supports_i_forward;
    return H2_PAL_OK;
}

static void h2_sctp_association_apply_peer_init(
    h2_pal_sctp_association_t *association,
    const h2_sctp_init_values_t *values) {
    association->peer_verification_tag = values->initiate_tag;
    association->peer_initial_tsn = values->initial_tsn;
    association->cumulative_received_tsn = values->initial_tsn - 1u;
    association->expected_reset_sequence = values->initial_tsn;
    association->peer_receive_window = values->receive_window;
    association->negotiated_outbound_streams = h2_sctp_min_u16(
        association->config.outbound_streams, values->inbound_streams);
    association->negotiated_inbound_streams = h2_sctp_min_u16(
        association->config.inbound_streams, values->outbound_streams);
    association->peer_forward_tsn = values->forward_tsn;
    association->peer_interleaving = values->interleaving;
    association->peer_stream_reset = values->stream_reset;
}

static h2_sctp_peer_state_t h2_sctp_association_peer_state(
    const h2_pal_sctp_association_t *association) {
    const h2_sctp_peer_state_t state = {
        .verification_tag = association->peer_verification_tag,
        .initial_tsn = association->peer_initial_tsn,
        .cumulative_received_tsn = association->cumulative_received_tsn,
        .expected_reset_sequence = association->expected_reset_sequence,
        .receive_window = association->peer_receive_window,
        .inbound_streams = association->negotiated_inbound_streams,
        .outbound_streams = association->negotiated_outbound_streams,
        .forward_tsn = association->peer_forward_tsn,
        .interleaving = association->peer_interleaving,
        .stream_reset = association->peer_stream_reset,
    };
    return state;
}

static void h2_sctp_association_restore_peer_state(
    h2_pal_sctp_association_t *association,
    const h2_sctp_peer_state_t *state) {
    association->peer_verification_tag = state->verification_tag;
    association->peer_initial_tsn = state->initial_tsn;
    association->cumulative_received_tsn = state->cumulative_received_tsn;
    association->expected_reset_sequence = state->expected_reset_sequence;
    association->peer_receive_window = state->receive_window;
    association->negotiated_inbound_streams = state->inbound_streams;
    association->negotiated_outbound_streams = state->outbound_streams;
    association->peer_forward_tsn = state->forward_tsn;
    association->peer_interleaving = state->interleaving;
    association->peer_stream_reset = state->stream_reset;
}

static h2_pal_result_t h2_sctp_association_send_init_ack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    h2_pal_result_t result = h2_sctp_association_random(
        association, association->cookie, sizeof(association->cookie));
    if (result != H2_PAL_OK) {
        return result;
    }
    association->cookie_valid = true;
    association->cookie_expires_ms = h2_sctp_deadline_add(
        now_ms, association->config.cookie_lifetime_ms);

    uint8_t chunk[68] = {0};
    chunk[0] = H2_SCTP_CHUNK_INIT_ACK;
    h2_sctp_wire_write_u32(chunk + 4u, association->local_verification_tag);
    const size_t receive_window = association->config.receive_buffer_size;
    h2_sctp_wire_write_u32(
        chunk + 8u,
        receive_window > UINT32_MAX ? UINT32_MAX : (uint32_t)receive_window);
    h2_sctp_wire_write_u16(
        chunk + 12u, association->config.outbound_streams);
    h2_sctp_wire_write_u16(
        chunk + 14u, association->config.inbound_streams);
    h2_sctp_wire_write_u32(chunk + 16u, association->initial_tsn);
    size_t offset = h2_sctp_association_write_extensions(chunk, 20u);
    h2_sctp_wire_write_u16(chunk + offset, H2_SCTP_PARAM_STATE_COOKIE);
    h2_sctp_wire_write_u16(
        chunk + offset + 2u, (uint16_t)(4u + sizeof(association->cookie)));
    memcpy(chunk + offset + 4u, association->cookie, sizeof(association->cookie));
    offset += 4u + sizeof(association->cookie);
    h2_sctp_wire_write_u16(chunk + 2u, (uint16_t)offset);
    return h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        chunk,
        offset,
        H2_SCTP_CONTROL_NONE,
        now_ms);
}

static h2_pal_result_t h2_sctp_association_handle_init(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    if (association->state != H2_PAL_SCTP_STATE_CONNECTING) {
        return H2_PAL_OK;
    }
    h2_sctp_init_values_t values;
    h2_pal_result_t result = h2_sctp_association_parse_init(chunk, &values);
    if (result != H2_PAL_OK) {
        return result;
    }
    const h2_sctp_peer_state_t peer_state =
        h2_sctp_association_peer_state(association);
    uint8_t previous_cookie[sizeof(association->cookie)];
    memcpy(previous_cookie, association->cookie, sizeof(previous_cookie));
    const bool previous_cookie_valid = association->cookie_valid;
    const uint64_t previous_cookie_expires_ms =
        association->cookie_expires_ms;
    h2_sctp_association_apply_peer_init(association, &values);
    result = h2_sctp_association_send_init_ack(association, now_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        h2_sctp_association_restore_peer_state(association, &peer_state);
        memcpy(association->cookie, previous_cookie, sizeof(previous_cookie));
        association->cookie_valid = previous_cookie_valid;
        association->cookie_expires_ms = previous_cookie_expires_ms;
        return result == H2_PAL_ERR_NO_MEMORY ? H2_PAL_ERR_WOULD_BLOCK
                                              : result;
    }
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

static h2_pal_result_t h2_sctp_association_handle_init_ack(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    if (association->config.role != H2_PAL_SCTP_ROLE_ACTIVE ||
        association->state != H2_PAL_SCTP_STATE_CONNECTING) {
        return H2_PAL_OK;
    }
    h2_sctp_init_values_t values;
    h2_pal_result_t result = h2_sctp_association_parse_init(chunk, &values);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (values.cookie == NULL || values.cookie_len == 0u ||
        values.cookie_len > association->config.max_packet_size - 16u) {
        return H2_PAL_ERR_FORMAT;
    }
    uint8_t *cookie = h2_sctp_alloc(association->owner, values.cookie_len);
    if (cookie == NULL) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    memcpy(cookie, values.cookie, values.cookie_len);

    const size_t chunk_len = 4u + values.cookie_len;
    const size_t padded_len = (chunk_len + 3u) & ~(size_t)3u;
    uint8_t *cookie_echo = h2_sctp_alloc(association->owner, padded_len);
    if (cookie_echo == NULL) {
        h2_sctp_free(association->owner, cookie);
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    cookie_echo[0] = H2_SCTP_CHUNK_COOKIE_ECHO;
    h2_sctp_wire_write_u16(cookie_echo + 2u, (uint16_t)chunk_len);
    memcpy(cookie_echo + 4u, values.cookie, values.cookie_len);
    const h2_sctp_peer_state_t peer_state =
        h2_sctp_association_peer_state(association);
    h2_sctp_association_apply_peer_init(association, &values);
    result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        cookie_echo,
        padded_len,
        H2_SCTP_CONTROL_COOKIE_ECHO,
        now_ms);
    h2_sctp_free(association->owner, cookie_echo);
    if (result == H2_PAL_ERR_NO_MEMORY) {
        h2_sctp_association_restore_peer_state(association, &peer_state);
        h2_sctp_free(association->owner, cookie);
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        h2_sctp_free(association->owner, cookie);
        return result;
    }
    h2_sctp_free(association->owner, association->peer_cookie);
    association->peer_cookie = cookie;
    association->peer_cookie_len = values.cookie_len;
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

static h2_pal_result_t h2_sctp_association_handle_cookie_echo(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    if (association->state != H2_PAL_SCTP_STATE_CONNECTING ||
        !association->cookie_valid || chunk->len != sizeof(association->cookie) ||
        now_ms > association->cookie_expires_ms ||
        memcmp(chunk->data, association->cookie, sizeof(association->cookie)) !=
            0) {
        return H2_PAL_OK;
    }
    uint8_t cookie_ack[4] = {H2_SCTP_CHUNK_COOKIE_ACK, 0u, 0u, 4u};
    h2_pal_result_t result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        cookie_ack,
        sizeof(cookie_ack),
        H2_SCTP_CONTROL_NONE,
        now_ms);
    if (result == H2_PAL_ERR_NO_MEMORY) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    association->cookie_valid = false;
    association->heartbeat_deadline_ms = h2_sctp_deadline_add(
        now_ms, H2_SCTP_HEARTBEAT_INTERVAL_MS);
    h2_sctp_notify_state(
        association, H2_PAL_SCTP_STATE_CONNECTED, H2_PAL_OK);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_sctp_association_handle_cookie_ack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    if (association->config.role != H2_PAL_SCTP_ROLE_ACTIVE ||
        association->state != H2_PAL_SCTP_STATE_CONNECTING) {
        return H2_PAL_OK;
    }
    h2_sctp_clear_control(association);
    association->heartbeat_deadline_ms = h2_sctp_deadline_add(
        now_ms, H2_SCTP_HEARTBEAT_INTERVAL_MS);
    h2_sctp_notify_state(
        association, H2_PAL_SCTP_STATE_CONNECTED, H2_PAL_OK);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_sctp_association_handle_heartbeat(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    if (chunk->len > association->config.max_packet_size - 16u) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t chunk_len = 4u + chunk->len;
    const size_t padded_len = (chunk_len + 3u) & ~(size_t)3u;
    uint8_t *reply = h2_sctp_alloc(association->owner, padded_len);
    if (reply == NULL) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    reply[0] = H2_SCTP_CHUNK_HEARTBEAT_ACK;
    h2_sctp_wire_write_u16(reply + 2u, (uint16_t)chunk_len);
    memcpy(reply + 4u, chunk->data, chunk->len);
    h2_pal_result_t result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        reply,
        padded_len,
        H2_SCTP_CONTROL_NONE,
        now_ms);
    h2_sctp_free(association->owner, reply);
    if (result == H2_PAL_ERR_NO_MEMORY) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

static h2_pal_result_t h2_sctp_association_handle_shutdown(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    if (chunk->len != 4u) {
        return H2_PAL_ERR_FORMAT;
    }
    const uint32_t cumulative = h2_sctp_wire_read_u32(chunk->data);
    h2_sctp_reliability_acknowledge_through(association, cumulative);
    association->peer_shutdown_pending = true;
    h2_sctp_notify_state(
        association, H2_PAL_SCTP_STATE_SHUTTING_DOWN, H2_PAL_OK);
    if (association->send_used != 0u) {
        return H2_PAL_OK;
    }
    association->peer_shutdown_pending = false;
    h2_pal_result_t result = h2_sctp_association_send_shutdown_ack(
        association, now_ms);
    if (result == H2_PAL_ERR_NO_MEMORY) {
        association->peer_shutdown_pending = true;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    return result;
}

static h2_pal_result_t h2_sctp_association_handle_shutdown_ack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    if (association->state != H2_PAL_SCTP_STATE_SHUTTING_DOWN) {
        return H2_PAL_OK;
    }
    uint8_t complete[4] = {
        H2_SCTP_CHUNK_SHUTDOWN_COMPLETE, 0u, 0u, 4u};
    h2_pal_result_t result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        complete,
        sizeof(complete),
        H2_SCTP_CONTROL_NONE,
        now_ms);
    if (result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK) {
        h2_sctp_clear_control(association);
        h2_sctp_notify_state(
            association, H2_PAL_SCTP_STATE_CLOSED, H2_PAL_OK);
        return H2_PAL_OK;
    }
    return result == H2_PAL_ERR_NO_MEMORY ? H2_PAL_ERR_WOULD_BLOCK : result;
}

static h2_pal_result_t h2_sctp_association_dispatch_chunk(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    switch (chunk->type) {
        case H2_SCTP_CHUNK_INIT:
            return h2_sctp_association_handle_init(association, chunk, now_ms);
        case H2_SCTP_CHUNK_INIT_ACK:
            return h2_sctp_association_handle_init_ack(
                association, chunk, now_ms);
        case H2_SCTP_CHUNK_COOKIE_ECHO:
            return h2_sctp_association_handle_cookie_echo(
                association, chunk, now_ms);
        case H2_SCTP_CHUNK_COOKIE_ACK:
            if (chunk->len != 0u) {
                return H2_PAL_ERR_FORMAT;
            }
            return h2_sctp_association_handle_cookie_ack(association, now_ms);
        case H2_SCTP_CHUNK_DATA:
        case H2_SCTP_CHUNK_I_DATA:
            if (association->state != H2_PAL_SCTP_STATE_CONNECTED &&
                association->state != H2_PAL_SCTP_STATE_SHUTTING_DOWN) {
                return H2_PAL_OK;
            }
            return h2_sctp_stream_handle_data(association, chunk, now_ms);
        case H2_SCTP_CHUNK_SACK:
            return h2_sctp_reliability_handle_sack(association, chunk, now_ms);
        case H2_SCTP_CHUNK_FORWARD_TSN:
        case H2_SCTP_CHUNK_I_FORWARD_TSN:
            return h2_sctp_stream_handle_forward_tsn(
                association, chunk, now_ms);
        case H2_SCTP_CHUNK_RE_CONFIG:
            return h2_sctp_stream_handle_reconfig(association, chunk, now_ms);
        case H2_SCTP_CHUNK_HEARTBEAT:
            return h2_sctp_association_handle_heartbeat(
                association, chunk, now_ms);
        case H2_SCTP_CHUNK_HEARTBEAT_ACK:
            association->heartbeat_deadline_ms = h2_sctp_deadline_add(
                now_ms, H2_SCTP_HEARTBEAT_INTERVAL_MS);
            return H2_PAL_OK;
        case H2_SCTP_CHUNK_SHUTDOWN:
            return h2_sctp_association_handle_shutdown(
                association, chunk, now_ms);
        case H2_SCTP_CHUNK_SHUTDOWN_ACK:
            return h2_sctp_association_handle_shutdown_ack(
                association, now_ms);
        case H2_SCTP_CHUNK_SHUTDOWN_COMPLETE:
            if (association->state == H2_PAL_SCTP_STATE_SHUTTING_DOWN) {
                h2_sctp_notify_state(
                    association, H2_PAL_SCTP_STATE_CLOSED, H2_PAL_OK);
            }
            return H2_PAL_OK;
        case H2_SCTP_CHUNK_ABORT:
            h2_sctp_fail(association, H2_PAL_ERR_CLOSED);
            return H2_PAL_OK;
        default:
            if ((chunk->type & 0xc0u) == 0u ||
                (chunk->type & 0xc0u) == 0x40u) {
                return H2_PAL_EXIT;
            }
            return H2_PAL_OK;
    }
}

h2_pal_result_t h2_sctp_association_start_impl(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    h2_pal_result_t result = h2_sctp_validate_operation(association, now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (association->state != H2_PAL_SCTP_STATE_NEW) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_sctp_notify_state(
        association, H2_PAL_SCTP_STATE_CONNECTING, H2_PAL_OK);
    if (association->config.role == H2_PAL_SCTP_ROLE_PASSIVE) {
        return H2_PAL_OK;
    }
    result = h2_sctp_association_send_init(association, now_ms);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        return H2_PAL_OK;
    }
    if (result != H2_PAL_OK) {
        h2_sctp_fail(association, result);
    }
    return result;
}

h2_pal_result_t h2_sctp_association_input_impl(
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len,
    uint64_t now_ms) {
    h2_pal_result_t result = h2_sctp_validate_operation(association, now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (association->state == H2_PAL_SCTP_STATE_CLOSED ||
        association->state == H2_PAL_SCTP_STATE_FAILED) {
        return H2_PAL_ERR_CLOSED;
    }
    /* max_packet_size is this endpoint's transmit budget. A peer may run a
     * larger path MTU, so inbound packets are bounded by the wire limit and
     * by the caller's own receive buffer, not by the local emit size. */
    if (packet_len > H2_SCTP_MAX_INBOUND_PACKET_SIZE) {
        return H2_PAL_ERR_FORMAT;
    }
    if (association->pending_emit != NULL) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    h2_sctp_packet_view_t packet_view;
    result = h2_sctp_wire_parse_packet(packet, packet_len, &packet_view);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (packet_view.source_port != association->config.remote_port ||
        packet_view.destination_port != association->config.local_port) {
        return H2_PAL_OK;
    }
    if (packet_view.chunks_len < 4u) {
        return H2_PAL_ERR_TRUNCATED;
    }
    const bool starts_with_init = packet_view.chunks[0] == H2_SCTP_CHUNK_INIT;
    if ((starts_with_init && packet_view.verification_tag != 0u) ||
        (!starts_with_init &&
         packet_view.verification_tag != association->local_verification_tag)) {
        return H2_PAL_OK;
    }
    size_t offset = 0u;
    while (offset < packet_view.chunks_len) {
        h2_sctp_chunk_view_t chunk;
        result = h2_sctp_wire_parse_chunk(
            packet_view.chunks, packet_view.chunks_len, offset, &chunk);
        if (result != H2_PAL_OK) {
            return result;
        }
        if ((chunk.type == H2_SCTP_CHUNK_INIT ||
             chunk.type == H2_SCTP_CHUNK_INIT_ACK) &&
            (offset != 0u || chunk.padded_len != packet_view.chunks_len)) {
            return H2_PAL_ERR_FORMAT;
        }
        offset += chunk.padded_len;
    }

    offset = 0u;
    while (offset < packet_view.chunks_len) {
        h2_sctp_chunk_view_t chunk;
        result = h2_sctp_wire_parse_chunk(
            packet_view.chunks, packet_view.chunks_len, offset, &chunk);
        if (result != H2_PAL_OK) {
            return result;
        }
        result = h2_sctp_association_dispatch_chunk(
            association, &chunk, now_ms);
        if (result == H2_PAL_EXIT) {
            return H2_PAL_OK;
        }
        if (result != H2_PAL_OK) {
            return result;
        }
        offset += chunk.padded_len;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_sctp_association_send_shutdown(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    uint8_t chunk[8] = {H2_SCTP_CHUNK_SHUTDOWN, 0u, 0u, 8u};
    h2_sctp_wire_write_u32(chunk + 4u, association->cumulative_received_tsn);
    h2_pal_result_t result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        chunk,
        sizeof(chunk),
        H2_SCTP_CONTROL_SHUTDOWN,
        now_ms);
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

h2_pal_result_t h2_sctp_association_send_shutdown_ack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    uint8_t reply[4] = {H2_SCTP_CHUNK_SHUTDOWN_ACK, 0u, 0u, 4u};
    h2_pal_result_t result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        reply,
        sizeof(reply),
        H2_SCTP_CONTROL_NONE,
        now_ms);
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

h2_pal_result_t h2_sctp_association_shutdown_impl(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    h2_pal_result_t result = h2_sctp_validate_operation(association, now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (association->state != H2_PAL_SCTP_STATE_CONNECTED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (association->send_used == 0u) {
        result = h2_sctp_association_send_shutdown(association, now_ms);
        if (result != H2_PAL_OK) {
            return result;
        }
        h2_sctp_notify_state(
            association, H2_PAL_SCTP_STATE_SHUTTING_DOWN, H2_PAL_OK);
        return H2_PAL_OK;
    }
    association->shutdown_pending = true;
    h2_sctp_notify_state(
        association, H2_PAL_SCTP_STATE_SHUTTING_DOWN, H2_PAL_OK);
    return H2_PAL_OK;
}

h2_pal_result_t h2_sctp_association_abort_impl(
    h2_pal_sctp_association_t *association,
    h2_pal_result_t reason,
    uint64_t now_ms) {
    h2_pal_result_t result = h2_sctp_validate_operation(association, now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (association->state == H2_PAL_SCTP_STATE_CLOSED ||
        association->state == H2_PAL_SCTP_STATE_FAILED) {
        return H2_PAL_ERR_CLOSED;
    }
    uint8_t chunk[4] = {H2_SCTP_CHUNK_ABORT, 0u, 0u, 4u};
    result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        chunk,
        sizeof(chunk),
        H2_SCTP_CONTROL_NONE,
        now_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    h2_sctp_fail(association, reason);
    return H2_PAL_OK;
}

h2_pal_result_t h2_sctp_association_reset_stream_impl(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    uint64_t now_ms) {
    h2_pal_result_t result = h2_sctp_validate_operation(association, now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (association->state != H2_PAL_SCTP_STATE_CONNECTED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (!association->peer_stream_reset) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (stream_id >= association->negotiated_outbound_streams) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_sctp_stream_t *stream = h2_sctp_stream_get_or_create(
        association, stream_id);
    if (stream == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (stream->reset_pending) {
        return H2_PAL_ERR_BUSY;
    }
    const uint32_t sequence = association->next_reset_sequence;
    uint8_t chunk[24] = {0};
    chunk[0] = H2_SCTP_CHUNK_RE_CONFIG;
    h2_sctp_wire_write_u16(chunk + 2u, 22u);
    h2_sctp_wire_write_u16(chunk + 4u, H2_SCTP_PARAM_OUTGOING_RESET);
    h2_sctp_wire_write_u16(chunk + 6u, 18u);
    h2_sctp_wire_write_u32(chunk + 8u, sequence);
    h2_sctp_wire_write_u32(
        chunk + 12u, association->expected_reset_sequence - 1u);
    h2_sctp_wire_write_u32(chunk + 16u, association->next_tsn - 1u);
    h2_sctp_wire_write_u16(chunk + 20u, stream_id);
    stream->reset_pending = true;
    stream->reset_request_sequence = sequence;
    result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        chunk,
        sizeof(chunk),
        H2_SCTP_CONTROL_RESET,
        now_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        stream->reset_pending = false;
        return result;
    }
    association->next_reset_sequence++;
    return H2_PAL_OK;
}
