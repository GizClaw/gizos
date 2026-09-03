#include "h2_sctp_reliability.h"

#include "h2_sctp_stream.h"

#include <limits.h>
#include <string.h>

static uint32_t h2_sctp_reliability_limit(
    const h2_pal_sctp_association_t *association) {
    uint32_t limit = association->cwnd;
    if (association->peer_receive_window < limit) {
        limit = association->peer_receive_window;
    }
    return limit;
}

static h2_sctp_tx_fragment_t *h2_sctp_reliability_pick_unsent(
    h2_pal_sctp_association_t *association) {
    h2_sctp_stream_t *after = NULL;
    h2_sctp_stream_t *wrapped = NULL;
    for (h2_sctp_stream_t *stream = association->streams;
         stream != NULL;
         stream = stream->next) {
        while (stream->tx_unsent != NULL &&
               (stream->tx_unsent->sent ||
                stream->tx_unsent->acknowledged ||
                stream->tx_unsent->abandoned)) {
            stream->tx_unsent = stream->tx_unsent->stream_next;
        }
        if (stream->tx_unsent == NULL) {
            continue;
        }
        if (wrapped == NULL || stream->id < wrapped->id) {
            wrapped = stream;
        }
        if (stream->id > association->last_scheduled_stream &&
            (after == NULL || stream->id < after->id)) {
            after = stream;
        }
    }
    h2_sctp_stream_t *selected = after != NULL ? after : wrapped;
    return selected == NULL ? NULL : selected->tx_unsent;
}

static h2_pal_result_t h2_sctp_reliability_emit_fragment(
    h2_pal_sctp_association_t *association,
    h2_sctp_tx_fragment_t *fragment,
    uint64_t now_ms,
    bool retransmission) {
    const bool interleaved = association->peer_interleaving;
    const size_t header_size = interleaved ? 20u : 16u;
    if (fragment->data == NULL ||
        fragment->data_len > SIZE_MAX - header_size) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const size_t chunk_len = header_size + fragment->data_len;
    const size_t padded_len = (chunk_len + 3u) & ~(size_t)3u;
    uint8_t *chunk = h2_sctp_alloc(association->owner, padded_len);
    if (chunk == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (!fragment->tsn_assigned) {
        fragment->tsn = association->next_tsn++;
        fragment->tsn_assigned = true;
    }
    chunk[0] = interleaved ? H2_SCTP_CHUNK_I_DATA : H2_SCTP_CHUNK_DATA;
    chunk[1] = fragment->flags;
    h2_sctp_wire_write_u16(chunk + 2u, (uint16_t)chunk_len);
    h2_sctp_wire_write_u32(chunk + 4u, fragment->tsn);
    h2_sctp_wire_write_u16(chunk + 8u, fragment->stream_id);
    if (interleaved) {
        h2_sctp_wire_write_u16(chunk + 10u, 0u);
        h2_sctp_wire_write_u32(chunk + 12u, fragment->message_identifier);
        h2_sctp_wire_write_u32(
            chunk + 16u,
            (fragment->flags & H2_SCTP_DATA_FLAG_BEGIN) != 0u
                ? fragment->ppid
                : fragment->fragment_sequence);
        memcpy(chunk + 20u, fragment->data, fragment->data_len);
    } else {
        h2_sctp_wire_write_u16(
            chunk + 10u, (uint16_t)fragment->message_identifier);
        h2_sctp_wire_write_u32(chunk + 12u, fragment->ppid);
        memcpy(chunk + 16u, fragment->data, fragment->data_len);
    }
    h2_pal_result_t result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        chunk,
        padded_len,
        H2_SCTP_CONTROL_NONE,
        now_ms);
    h2_sctp_free(association->owner, chunk);
    if (result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK) {
        if (!fragment->sent) {
            association->flight_size += fragment->data_len;
        }
        fragment->sent = true;
        fragment->sent_ms = now_ms;
        fragment->miss_reports = 0u;
        if (retransmission) {
            fragment->retransmits++;
        } else {
            h2_sctp_stream_t *stream = h2_sctp_stream_find(
                association, fragment->stream_id);
            if (stream != NULL && stream->tx_unsent == fragment) {
                stream->tx_unsent = fragment->stream_next;
            }
        }
    }
    return result;
}

h2_pal_result_t h2_sctp_reliability_send_pending(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    if (association->pending_emit != NULL) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    const uint32_t limit = h2_sctp_reliability_limit(association);
    for (;;) {
        h2_sctp_tx_fragment_t *fragment = h2_sctp_reliability_pick_unsent(
            association);
        if (fragment == NULL) {
            return H2_PAL_OK;
        }
        if (fragment->data_len > limit ||
            association->flight_size > limit - fragment->data_len) {
            return H2_PAL_OK;
        }
        h2_pal_result_t result = h2_sctp_reliability_emit_fragment(
            association, fragment, now_ms, false);
        if (result != H2_PAL_OK) {
            return result;
        }
        association->last_scheduled_stream = fragment->stream_id;
    }
}

static bool h2_sctp_reliability_tsn_in_gap(
    uint32_t tsn,
    uint32_t cumulative,
    const h2_sctp_chunk_view_t *chunk) {
    const uint16_t gap_count = h2_sctp_wire_read_u16(chunk->data + 8u);
    for (uint16_t index = 0u; index < gap_count; ++index) {
        const uint16_t start = h2_sctp_wire_read_u16(
            chunk->data + 12u + (size_t)index * 4u);
        const uint16_t end = h2_sctp_wire_read_u16(
            chunk->data + 14u + (size_t)index * 4u);
        const uint32_t start_tsn = cumulative + start;
        const uint32_t end_tsn = cumulative + end;
        if (!h2_sctp_tsn_before(tsn, start_tsn) &&
            !h2_sctp_tsn_after(tsn, end_tsn)) {
            return true;
        }
    }
    return false;
}

static bool h2_sctp_reliability_sack_is_valid(
    const h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint32_t cumulative) {
    uint32_t highest_sent = association->peer_cumulative_tsn;
    if (h2_sctp_tsn_after(
            association->advanced_peer_ack, highest_sent)) {
        highest_sent = association->advanced_peer_ack;
    }
    for (const h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (fragment->tsn_assigned && fragment->sent &&
            h2_sctp_tsn_after(fragment->tsn, highest_sent)) {
            highest_sent = fragment->tsn;
        }
    }
    if (h2_sctp_tsn_after(cumulative, highest_sent)) {
        return false;
    }

    const uint16_t gap_count = h2_sctp_wire_read_u16(chunk->data + 8u);
    uint16_t previous_end = 0u;
    for (uint16_t index = 0u; index < gap_count; ++index) {
        const uint16_t start = h2_sctp_wire_read_u16(
            chunk->data + 12u + (size_t)index * 4u);
        const uint16_t end = h2_sctp_wire_read_u16(
            chunk->data + 14u + (size_t)index * 4u);
        if (start == 0u || start > end || start <= previous_end ||
            h2_sctp_tsn_after(cumulative + end, highest_sent)) {
            return false;
        }
        previous_end = end;
    }
    return true;
}

static void h2_sctp_reliability_mark_acknowledged(
    h2_pal_sctp_association_t *association,
    h2_sctp_tx_fragment_t *fragment,
    size_t *in_out_acked_bytes) {
    if (fragment->acknowledged || fragment->abandoned) {
        return;
    }
    fragment->acknowledged = true;
    if (fragment->sent && association->flight_size >= fragment->data_len) {
        association->flight_size -= fragment->data_len;
    }
    if (association->send_used >= fragment->data_len) {
        association->send_used -= fragment->data_len;
    }
    *in_out_acked_bytes += fragment->data_len;
    h2_sctp_free(association->owner, fragment->data);
    fragment->data = NULL;
}

static void h2_sctp_reliability_prune(
    h2_pal_sctp_association_t *association,
    uint32_t through_tsn) {
    h2_sctp_tx_fragment_t **cursor = &association->tx_fragments;
    h2_sctp_tx_fragment_t *previous = NULL;
    while (*cursor != NULL) {
        h2_sctp_tx_fragment_t *fragment = *cursor;
        if (fragment->tsn_assigned &&
            !h2_sctp_tsn_after(fragment->tsn, through_tsn) &&
            (fragment->acknowledged || fragment->abandoned)) {
            *cursor = fragment->next;
            h2_sctp_stream_t *stream = h2_sctp_stream_find(
                association, fragment->stream_id);
            if (stream != NULL) {
                if (stream->tx_unsent == fragment) {
                    stream->tx_unsent = fragment->stream_next;
                }
                if (stream->tx_tail == fragment) {
                    stream->tx_tail = NULL;
                }
            }
            if (association->tx_fragments_tail == fragment) {
                association->tx_fragments_tail = previous;
            }
            h2_sctp_free(association->owner, fragment->data);
            h2_sctp_free(association->owner, fragment);
            continue;
        }
        previous = fragment;
        cursor = &fragment->next;
    }
}

void h2_sctp_reliability_acknowledge_through(
    h2_pal_sctp_association_t *association,
    uint32_t cumulative_tsn) {
    size_t acknowledged_bytes = 0u;
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (fragment->tsn_assigned &&
            !h2_sctp_tsn_after(fragment->tsn, cumulative_tsn)) {
            h2_sctp_reliability_mark_acknowledged(
                association, fragment, &acknowledged_bytes);
        }
    }
    if (h2_sctp_tsn_after(cumulative_tsn, association->peer_cumulative_tsn)) {
        association->peer_cumulative_tsn = cumulative_tsn;
    }
    h2_sctp_reliability_prune(association, cumulative_tsn);
}

h2_pal_result_t h2_sctp_reliability_handle_sack(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    if (chunk->len < 12u) {
        return H2_PAL_ERR_TRUNCATED;
    }
    const uint16_t gap_count = h2_sctp_wire_read_u16(chunk->data + 8u);
    const uint16_t duplicate_count = h2_sctp_wire_read_u16(chunk->data + 10u);
    const size_t entry_bytes =
        ((size_t)gap_count + (size_t)duplicate_count) * 4u;
    if (entry_bytes != chunk->len - 12u) {
        return H2_PAL_ERR_FORMAT;
    }
    const uint32_t cumulative = h2_sctp_wire_read_u32(chunk->data);
    if (h2_sctp_tsn_before(
            cumulative, association->peer_cumulative_tsn)) {
        return H2_PAL_OK;
    }
    if (!h2_sctp_reliability_sack_is_valid(
            association, chunk, cumulative)) {
        return H2_PAL_ERR_FORMAT;
    }
    association->peer_receive_window = h2_sctp_wire_read_u32(
        chunk->data + 4u);
    uint32_t highest_acknowledged = cumulative;
    for (uint16_t index = 0u; index < gap_count; ++index) {
        const uint16_t end = h2_sctp_wire_read_u16(
            chunk->data + 14u + (size_t)index * 4u);
        const uint32_t end_tsn = cumulative + end;
        if (h2_sctp_tsn_after(end_tsn, highest_acknowledged)) {
            highest_acknowledged = end_tsn;
        }
    }
    size_t acked_bytes = 0u;
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (!fragment->tsn_assigned || fragment->acknowledged ||
            fragment->abandoned) {
            continue;
        }
        if (!h2_sctp_tsn_after(fragment->tsn, cumulative) ||
            h2_sctp_reliability_tsn_in_gap(
                fragment->tsn, cumulative, chunk)) {
            h2_sctp_reliability_mark_acknowledged(
                association, fragment, &acked_bytes);
        } else if (h2_sctp_tsn_before(fragment->tsn, highest_acknowledged)) {
            fragment->miss_reports++;
            if (fragment->miss_reports >= 3u) {
                fragment->sent_ms = now_ms >= association->rto_ms
                                        ? now_ms - association->rto_ms
                                        : 0u;
            }
        }
    }
    if (h2_sctp_tsn_after(cumulative, association->peer_cumulative_tsn)) {
        association->peer_cumulative_tsn = cumulative;
    }
    if (h2_sctp_tsn_after(cumulative, association->advanced_peer_ack)) {
        association->advanced_peer_ack = cumulative;
    }
    h2_sctp_reliability_prune(association, cumulative);
    if (acked_bytes != 0u) {
        if (association->cwnd < association->ssthresh) {
            const size_t increase =
                acked_bytes < association->config.max_packet_size
                    ? acked_bytes
                    : association->config.max_packet_size;
            association->cwnd =
                increase > UINT32_MAX - association->cwnd
                    ? UINT32_MAX
                    : association->cwnd + (uint32_t)increase;
        } else if (association->cwnd != 0u) {
            const uint64_t increase =
                (uint64_t)association->config.max_packet_size *
                association->config.max_packet_size / association->cwnd;
            association->cwnd =
                increase > UINT32_MAX - association->cwnd
                    ? UINT32_MAX
                    : association->cwnd + (uint32_t)(increase == 0u ? 1u
                                                                    : increase);
        }
    }
    h2_pal_result_t result = h2_sctp_reliability_service(
        association, now_ms, NULL);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK &&
        result != H2_PAL_ERR_NO_MEMORY) {
        return result;
    }
    result = h2_sctp_reliability_send_pending(association, now_ms);
    return result == H2_PAL_ERR_WOULD_BLOCK ||
                   result == H2_PAL_ERR_NO_MEMORY
               ? H2_PAL_OK
               : result;
}

static void h2_sctp_reliability_abandon_message(
    h2_pal_sctp_association_t *association,
    uint64_t message_id) {
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (fragment->message_id != message_id || fragment->acknowledged ||
            fragment->abandoned) {
            continue;
        }
        fragment->abandoned = true;
        if (fragment->sent && association->flight_size >= fragment->data_len) {
            association->flight_size -= fragment->data_len;
        }
        if (association->send_used >= fragment->data_len) {
            association->send_used -= fragment->data_len;
        }
        h2_sctp_free(association->owner, fragment->data);
        fragment->data = NULL;
    }
}

static h2_sctp_tx_fragment_t *h2_sctp_reliability_find_tsn(
    h2_pal_sctp_association_t *association,
    uint32_t tsn) {
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (fragment->tsn_assigned && fragment->tsn == tsn) {
            return fragment;
        }
    }
    return NULL;
}

static h2_pal_result_t h2_sctp_reliability_emit_forward_tsn(
    h2_pal_sctp_association_t *association,
    uint32_t new_cumulative,
    uint64_t now_ms) {
    h2_sctp_tx_fragment_t *representative = NULL;
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (fragment->abandoned && fragment->tsn_assigned &&
            !h2_sctp_tsn_after(fragment->tsn, new_cumulative)) {
            representative = fragment;
        }
    }
    const bool interleaved = association->peer_interleaving;
    uint8_t chunk[16] = {0};
    const size_t chunk_len = representative == NULL ? 8u
                                                     : (interleaved ? 16u : 12u);
    chunk[0] = interleaved ? H2_SCTP_CHUNK_I_FORWARD_TSN
                           : H2_SCTP_CHUNK_FORWARD_TSN;
    h2_sctp_wire_write_u16(chunk + 2u, (uint16_t)chunk_len);
    h2_sctp_wire_write_u32(chunk + 4u, new_cumulative);
    if (representative != NULL) {
        h2_sctp_wire_write_u16(chunk + 8u, representative->stream_id);
        if (interleaved) {
            h2_sctp_wire_write_u16(
                chunk + 10u,
                (representative->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u
                    ? 1u
                    : 0u);
            h2_sctp_wire_write_u32(
                chunk + 12u, representative->message_identifier);
        } else {
            h2_sctp_wire_write_u16(
                chunk + 10u, (uint16_t)representative->message_identifier);
        }
    }
    return h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        chunk,
        chunk_len,
        H2_SCTP_CONTROL_NONE,
        now_ms);
}

static h2_pal_result_t h2_sctp_reliability_advance_abandoned(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    uint32_t advanced = association->peer_cumulative_tsn;
    bool included_abandoned = false;
    for (;;) {
        h2_sctp_tx_fragment_t *fragment = h2_sctp_reliability_find_tsn(
            association, advanced + 1u);
        if (fragment == NULL ||
            (!fragment->acknowledged && !fragment->abandoned)) {
            break;
        }
        advanced++;
        included_abandoned = included_abandoned || fragment->abandoned;
    }
    if (!included_abandoned ||
        !h2_sctp_tsn_after(advanced, association->advanced_peer_ack)) {
        return H2_PAL_OK;
    }
    h2_pal_result_t result = h2_sctp_reliability_emit_forward_tsn(
        association, advanced, now_ms);
    if (result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK) {
        association->advanced_peer_ack = advanced;
        h2_sctp_reliability_prune(association, advanced);
    }
    return result;
}

h2_pal_result_t h2_sctp_reliability_service(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *in_out_deadline_ms) {
    uint64_t deadline = in_out_deadline_ms == NULL
                            ? H2_PAL_SCTP_NO_DEADLINE
                            : *in_out_deadline_ms;
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (!fragment->sent || fragment->acknowledged || fragment->abandoned) {
            continue;
        }
        const uint64_t retransmit_deadline = h2_sctp_deadline_add(
            fragment->sent_ms, association->rto_ms);
        if (now_ms < retransmit_deadline) {
            if (deadline == H2_PAL_SCTP_NO_DEADLINE ||
                retransmit_deadline < deadline) {
                deadline = retransmit_deadline;
            }
            continue;
        }
        const bool lifetime_expired =
            fragment->reliability ==
                H2_PAL_SCTP_RELIABILITY_MAX_LIFETIME_MS &&
            now_ms - fragment->submitted_ms >= fragment->reliability_value;
        const bool retransmits_exhausted =
            fragment->reliability ==
                H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS &&
            fragment->retransmits >= fragment->reliability_value;
        if (lifetime_expired || retransmits_exhausted) {
            const uint64_t message_id = fragment->message_id;
            h2_sctp_reliability_abandon_message(association, message_id);
            continue;
        }
        if (association->pending_emit != NULL) {
            deadline = now_ms;
            break;
        }
        association->ssthresh = association->cwnd / 2u;
        const size_t minimum_ssthresh =
            association->config.max_packet_size > SIZE_MAX / 4u
                ? SIZE_MAX
                : association->config.max_packet_size * 4u;
        const uint32_t bounded_minimum_ssthresh =
            minimum_ssthresh > UINT32_MAX ? UINT32_MAX
                                          : (uint32_t)minimum_ssthresh;
        if (association->ssthresh < bounded_minimum_ssthresh) {
            association->ssthresh = bounded_minimum_ssthresh;
        }
        association->cwnd = association->config.max_packet_size > UINT32_MAX
                                ? UINT32_MAX
                                : (uint32_t)association->config.max_packet_size;
        h2_pal_result_t result = h2_sctp_reliability_emit_fragment(
            association, fragment, now_ms, true);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
            return result;
        }
        if (association->rto_ms < H2_SCTP_RTO_MAX_MS / 2u) {
            association->rto_ms *= 2u;
        } else {
            association->rto_ms = H2_SCTP_RTO_MAX_MS;
        }
        if (result == H2_PAL_ERR_WOULD_BLOCK) {
            deadline = now_ms;
            break;
        }
    }
    h2_pal_result_t result = h2_sctp_reliability_advance_abandoned(
        association, now_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        deadline = now_ms;
    }
    if (in_out_deadline_ms != NULL) {
        *in_out_deadline_ms = deadline;
    }
    return result;
}

h2_pal_result_t h2_sctp_reliability_send_sack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    const bool emit_available = association->pending_emit == NULL;
    uint8_t chunk[80] = {0};
    chunk[0] = H2_SCTP_CHUNK_SACK;
    h2_sctp_wire_write_u32(
        chunk + 4u, association->cumulative_received_tsn);
    const size_t available = association->config.receive_buffer_size -
                             association->receive_used;
    h2_sctp_wire_write_u32(
        chunk + 8u, available > UINT32_MAX ? UINT32_MAX : (uint32_t)available);

    uint16_t gap_count = 0u;
    h2_sctp_rx_fragment_t *fragment = association->rx_fragments;
    while (fragment != NULL && gap_count < 16u) {
        if (!h2_sctp_tsn_after(
                fragment->tsn, association->cumulative_received_tsn)) {
            fragment = fragment->next;
            continue;
        }
        const uint32_t start_offset =
            fragment->tsn - association->cumulative_received_tsn;
        uint32_t end_offset = start_offset;
        h2_sctp_rx_fragment_t *next = fragment->next;
        while (next != NULL && next->tsn == fragment->tsn + 1u) {
            fragment = next;
            end_offset = fragment->tsn - association->cumulative_received_tsn;
            next = fragment->next;
        }
        if (start_offset > UINT16_MAX || end_offset > UINT16_MAX) {
            break;
        }
        h2_sctp_wire_write_u16(
            chunk + 16u + (size_t)gap_count * 4u,
            (uint16_t)start_offset);
        h2_sctp_wire_write_u16(
            chunk + 18u + (size_t)gap_count * 4u,
            (uint16_t)end_offset);
        gap_count++;
        fragment = next;
    }
    h2_sctp_wire_write_u16(chunk + 12u, gap_count);
    h2_sctp_wire_write_u16(chunk + 14u, 0u);
    const size_t chunk_len = 16u + (size_t)gap_count * 4u;
    h2_sctp_wire_write_u16(chunk + 2u, (uint16_t)chunk_len);
    const h2_pal_result_t result = h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        chunk,
        chunk_len,
        H2_SCTP_CONTROL_NONE,
        now_ms);
    if (emit_available &&
        (result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK)) {
        association->sack_pending_packets = 0u;
        association->sack_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
    }
    return result;
}

h2_pal_result_t h2_sctp_reliability_note_data(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    bool immediate) {
    if (association->sack_pending_packets < UINT8_MAX) {
        association->sack_pending_packets++;
    }
    if (association->sack_deadline_ms == H2_PAL_SCTP_NO_DEADLINE) {
        association->sack_deadline_ms = h2_sctp_deadline_add(
            now_ms, H2_SCTP_DELAYED_SACK_MS);
    }
    if (!immediate &&
        association->sack_pending_packets < H2_SCTP_DELAYED_SACK_PACKETS) {
        return H2_PAL_OK;
    }
    association->sack_deadline_ms = now_ms;
    if (association->pending_emit != NULL) {
        return H2_PAL_OK;
    }
    return h2_sctp_reliability_send_sack(association, now_ms);
}

h2_pal_result_t h2_sctp_reliability_service_sack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *in_out_deadline_ms) {
    if (association->sack_pending_packets == 0u ||
        association->sack_deadline_ms == H2_PAL_SCTP_NO_DEADLINE) {
        return H2_PAL_OK;
    }
    if (now_ms < association->sack_deadline_ms) {
        if (*in_out_deadline_ms == H2_PAL_SCTP_NO_DEADLINE ||
            association->sack_deadline_ms < *in_out_deadline_ms) {
            *in_out_deadline_ms = association->sack_deadline_ms;
        }
        return H2_PAL_OK;
    }
    if (association->pending_emit != NULL) {
        *in_out_deadline_ms = now_ms;
        return H2_PAL_OK;
    }
    const h2_pal_result_t result = h2_sctp_reliability_send_sack(
        association, now_ms);
    if (result == H2_PAL_ERR_NO_MEMORY) {
        *in_out_deadline_ms = now_ms;
        return H2_PAL_OK;
    }
    return result;
}

void h2_sctp_reliability_release_all(
    h2_pal_sctp_association_t *association) {
    h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
    while (fragment != NULL) {
        h2_sctp_tx_fragment_t *next = fragment->next;
        h2_sctp_free(association->owner, fragment->data);
        h2_sctp_free(association->owner, fragment);
        fragment = next;
    }
    association->tx_fragments = NULL;
    association->tx_fragments_tail = NULL;
    for (h2_sctp_stream_t *stream = association->streams;
         stream != NULL;
         stream = stream->next) {
        stream->tx_unsent = NULL;
        stream->tx_tail = NULL;
    }
    association->send_used = 0u;
    association->flight_size = 0u;
}
