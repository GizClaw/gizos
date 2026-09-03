#include "h2_sctp_timer.h"

#include "h2_sctp_association.h"
#include "h2_sctp_reliability.h"
#include "h2_sctp_stream.h"
#include "h2_sctp_wire.h"

static void h2_sctp_timer_min_deadline(
    uint64_t candidate,
    uint64_t *in_out_deadline) {
    if (candidate != H2_PAL_SCTP_NO_DEADLINE &&
        (*in_out_deadline == H2_PAL_SCTP_NO_DEADLINE ||
         candidate < *in_out_deadline)) {
        *in_out_deadline = candidate;
    }
}

static h2_pal_result_t h2_sctp_timer_emit_saved_control(
    h2_pal_sctp_association_t *association) {
    association->in_callback = true;
    const h2_pal_result_t result = association->config.callbacks.emit_packet(
        association->config.callbacks.user,
        association,
        association->control_packet,
        association->control_packet_len);
    association->in_callback = false;
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        h2_sctp_fail(association, result);
    }
    return result;
}

static h2_pal_result_t h2_sctp_timer_service_control(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *in_out_deadline) {
    if (association->control_packet == NULL ||
        association->control_deadline_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_OK;
    }
    if (now_ms < association->control_deadline_ms) {
        h2_sctp_timer_min_deadline(
            association->control_deadline_ms, in_out_deadline);
        return H2_PAL_OK;
    }
    if (association->control_retries >= H2_SCTP_MAX_CONTROL_RETRIES) {
        h2_sctp_fail(association, H2_PAL_ERR_TIMEOUT);
        return H2_PAL_ERR_TIMEOUT;
    }
    const h2_pal_result_t result = h2_sctp_timer_emit_saved_control(association);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        *in_out_deadline = now_ms;
        return H2_PAL_OK;
    }
    if (result != H2_PAL_OK) {
        return result;
    }
    association->control_retries++;
    if (association->rto_ms < H2_SCTP_RTO_MAX_MS / 2u) {
        association->rto_ms *= 2u;
    } else {
        association->rto_ms = H2_SCTP_RTO_MAX_MS;
    }
    association->control_deadline_ms = h2_sctp_deadline_add(
        now_ms, association->rto_ms);
    h2_sctp_timer_min_deadline(
        association->control_deadline_ms, in_out_deadline);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_sctp_timer_service_heartbeat(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *in_out_deadline) {
    if (association->state != H2_PAL_SCTP_STATE_CONNECTED) {
        return H2_PAL_OK;
    }
    if (association->heartbeat_deadline_ms == H2_PAL_SCTP_NO_DEADLINE) {
        association->heartbeat_deadline_ms = h2_sctp_deadline_add(
            now_ms, H2_SCTP_HEARTBEAT_INTERVAL_MS);
    }
    if (now_ms < association->heartbeat_deadline_ms) {
        h2_sctp_timer_min_deadline(
            association->heartbeat_deadline_ms, in_out_deadline);
        return H2_PAL_OK;
    }
    uint8_t heartbeat[16] = {H2_SCTP_CHUNK_HEARTBEAT, 0u, 0u, 16u};
    h2_sctp_wire_write_u16(heartbeat + 4u, 1u);
    h2_sctp_wire_write_u16(heartbeat + 6u, 12u);
    h2_sctp_wire_write_u32(heartbeat + 8u, (uint32_t)(now_ms >> 32u));
    h2_sctp_wire_write_u32(heartbeat + 12u, (uint32_t)now_ms);
    h2_pal_result_t result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        heartbeat,
        sizeof(heartbeat),
        H2_SCTP_CONTROL_NONE,
        now_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    association->heartbeat_deadline_ms = h2_sctp_deadline_add(
        now_ms, H2_SCTP_HEARTBEAT_INTERVAL_MS);
    h2_sctp_timer_min_deadline(
        association->heartbeat_deadline_ms, in_out_deadline);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        *in_out_deadline = now_ms;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_sctp_timer_service(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *out_next_deadline_ms) {
    h2_pal_result_t result = h2_sctp_validate_operation(association, now_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    *out_next_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
    result = h2_sctp_retry_pending_emit(association);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        *out_next_deadline_ms = now_ms;
        return H2_PAL_OK;
    }
    if (result != H2_PAL_OK) {
        return result;
    }
    if (association->state == H2_PAL_SCTP_STATE_CLOSED ||
        association->state == H2_PAL_SCTP_STATE_FAILED) {
        return H2_PAL_OK;
    }

    result = h2_sctp_stream_service(association);
    if (result == H2_PAL_ERR_NO_MEMORY) {
        *out_next_deadline_ms = now_ms;
    } else if (result != H2_PAL_OK) {
        return result;
    }

    result = h2_sctp_reliability_service_sack(
        association, now_ms, out_next_deadline_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    if (association->pending_emit != NULL) {
        *out_next_deadline_ms = now_ms;
        return H2_PAL_OK;
    }

    result = h2_sctp_timer_service_control(
        association, now_ms, out_next_deadline_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = h2_sctp_reliability_service(
        association, now_ms, out_next_deadline_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    if (association->pending_emit != NULL) {
        *out_next_deadline_ms = now_ms;
        return H2_PAL_OK;
    }
    result = h2_sctp_reliability_send_pending(association, now_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    if (association->pending_emit != NULL || result == H2_PAL_ERR_WOULD_BLOCK) {
        *out_next_deadline_ms = now_ms;
        return H2_PAL_OK;
    }
    if (association->shutdown_pending && association->send_used == 0u &&
        association->control_kind == H2_SCTP_CONTROL_NONE) {
        association->shutdown_pending = false;
        result = h2_sctp_association_send_shutdown(association, now_ms);
        if (result != H2_PAL_OK) {
            association->shutdown_pending = true;
            return result;
        }
    }
    if (association->peer_shutdown_pending && association->send_used == 0u &&
        association->control_kind == H2_SCTP_CONTROL_NONE) {
        association->peer_shutdown_pending = false;
        result = h2_sctp_association_send_shutdown_ack(association, now_ms);
        if (result != H2_PAL_OK) {
            association->peer_shutdown_pending = true;
            return result;
        }
    }
    result = h2_sctp_timer_service_heartbeat(
        association, now_ms, out_next_deadline_ms);
    if (association->pending_emit != NULL) {
        *out_next_deadline_ms = now_ms;
    }
    return result;
}
