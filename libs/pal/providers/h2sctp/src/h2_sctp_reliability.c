#include "h2_sctp_reliability.h"

#include "h2_sctp_stream.h"

#include <limits.h>
#include <string.h>

/* floor(((denominator - 1) * previous + sample) / denominator),
 * without overflowing even for a very large caller-supplied timestamp. */
static uint64_t h2_sctp_rtt_weighted_average(uint64_t previous, uint64_t sample,
                                             uint64_t denominator) {
  return (previous / denominator) * (denominator - 1u) + sample / denominator +
         ((previous % denominator) * (denominator - 1u) +
          sample % denominator) /
             denominator;
}

static void
h2_sctp_reliability_sample_rtt(h2_pal_sctp_association_t *association,
                               const h2_sctp_tx_fragment_t *fragment,
                               uint64_t now_ms) {
  if (!association->rtt_sample_pending ||
      fragment->tsn != association->rtt_sample_tsn) {
    return;
  }
  association->rtt_sample_pending = false;
  if (now_ms < association->rtt_sample_sent_ms) {
    return;
  }
  const uint64_t sample = now_ms - association->rtt_sample_sent_ms;
  /* RFC 9260 section 6.3.1: alpha=1/8, beta=1/4, clock granularity=1ms. */
  if (!association->rtt_initialized) {
    association->srtt_ms = sample;
    association->rttvar_ms = sample / 2u;
    association->rtt_initialized = true;
  } else {
    const uint64_t delta = association->srtt_ms > sample
                               ? association->srtt_ms - sample
                               : sample - association->srtt_ms;
    association->rttvar_ms =
        h2_sctp_rtt_weighted_average(association->rttvar_ms, delta, 4u);
    association->srtt_ms =
        h2_sctp_rtt_weighted_average(association->srtt_ms, sample, 8u);
  }
  if (association->rttvar_ms == 0u) {
    association->rttvar_ms = 1u;
  }
  if (association->srtt_ms >= H2_SCTP_RTO_MAX_MS ||
      association->rttvar_ms >= H2_SCTP_RTO_MAX_MS / 4u) {
    association->rto_ms = H2_SCTP_RTO_MAX_MS;
  } else {
    const uint64_t rto = association->srtt_ms + 4u * association->rttvar_ms;
    association->rto_ms =
        rto < H2_SCTP_RTO_MIN_MS
            ? H2_SCTP_RTO_MIN_MS
            : (rto > H2_SCTP_RTO_MAX_MS ? H2_SCTP_RTO_MAX_MS : rto);
  }
}

static uint32_t h2_sctp_reliability_limit(
    const h2_pal_sctp_association_t *association) {
    uint32_t limit = association->cwnd;
    if (association->peer_receive_window < limit) {
        limit = association->peer_receive_window;
    }
    return limit;
}

/* RFC 9260 section 7.2.3: ssthresh = max(cwnd / 2, 4 * MTU). */
static uint32_t h2_sctp_reliability_reduced_ssthresh(
    const h2_pal_sctp_association_t *association) {
    const size_t minimum = association->config.max_packet_size > SIZE_MAX / 4u
                               ? SIZE_MAX
                               : association->config.max_packet_size * 4u;
    const uint32_t bounded_minimum =
        minimum > UINT32_MAX ? UINT32_MAX : (uint32_t)minimum;
    const uint32_t halved = association->cwnd / 2u;
    return halved < bounded_minimum ? bounded_minimum : halved;
}

static uint32_t h2_sctp_reliability_one_packet(
    const h2_pal_sctp_association_t *association) {
    return association->config.max_packet_size > UINT32_MAX
               ? UINT32_MAX
               : (uint32_t)association->config.max_packet_size;
}

/* A partially reliable fragment whose lifetime or retransmission budget is
 * spent must be abandoned instead of retransmitted. */
static bool h2_sctp_reliability_outlived(
    const h2_sctp_tx_fragment_t *fragment,
    uint64_t now_ms) {
    if (fragment->reliability == H2_PAL_SCTP_RELIABILITY_MAX_LIFETIME_MS) {
        return now_ms >= fragment->submitted_ms &&
               now_ms - fragment->submitted_ms >= fragment->reliability_value;
    }
    return fragment->reliability == H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS &&
           fragment->retransmits >= fragment->reliability_value;
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
        fragment->fast_retransmit = false;
        if (retransmission) {
            fragment->retransmits++;
            /* Karn: retransmitting this TSN or an earlier TSN makes the
             * pending observation ambiguous, including across TSN wrap. */
            if (association->rtt_sample_pending &&
                !h2_sctp_tsn_after(fragment->tsn,
                                   association->rtt_sample_tsn)) {
              association->rtt_sample_pending = false;
            }
        } else {
          /* A WOULD_BLOCK packet is locally buffered, not yet on the wire;
           * do not include its unknown local queue delay in a sample. */
          if (result == H2_PAL_OK && !association->rtt_sample_pending) {
            association->rtt_sample_pending = true;
            association->rtt_sample_tsn = fragment->tsn;
            association->rtt_sample_sent_ms = now_ms;
          }
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
    if (association->rtt_sample_pending &&
        fragment->tsn == association->rtt_sample_tsn) {
      association->rtt_sample_pending = false;
    }
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

static void h2_sctp_reliability_abandon_message(
    h2_pal_sctp_association_t *association,
    uint64_t message_id);

/*
 * RFC 9260 section 7.2.4: on the first fast retransmit of a loss event set
 * ssthresh = max(cwnd / 2, 4 * MTU) and cwnd = ssthresh, retransmit the marked
 * TSNs, and leave the RTO alone - only a timer expiry backs it off. Further
 * gap reports for the same hole arrive while fast recovery is active and must
 * not reduce the window again within that round trip.
 */
static h2_pal_result_t h2_sctp_reliability_fast_retransmit(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    bool marked = false;
    uint32_t exit_tsn = association->peer_cumulative_tsn;
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (!fragment->sent || fragment->acknowledged || fragment->abandoned) {
            continue;
        }
        marked = marked || fragment->fast_retransmit;
        if (h2_sctp_tsn_after(fragment->tsn, exit_tsn)) {
            exit_tsn = fragment->tsn;
        }
    }
    if (!marked) {
        return H2_PAL_OK;
    }
    if (!association->fast_recovery_active) {
        association->ssthresh =
            h2_sctp_reliability_reduced_ssthresh(association);
        association->cwnd = association->ssthresh;
        association->fast_recovery_active = true;
        association->fast_recovery_exit_tsn = exit_tsn;
    }
    h2_pal_result_t outcome = H2_PAL_OK;
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (!fragment->fast_retransmit || !fragment->sent ||
            fragment->acknowledged || fragment->abandoned) {
            continue;
        }
        if (h2_sctp_reliability_outlived(fragment, now_ms)) {
            fragment->fast_retransmit = false;
            h2_sctp_reliability_abandon_message(
                association, fragment->message_id);
            continue;
        }
        if (association->pending_emit != NULL) {
            outcome = H2_PAL_ERR_WOULD_BLOCK;
            break;
        }
        const h2_pal_result_t result = h2_sctp_reliability_emit_fragment(
            association, fragment, now_ms, true);
        if (result == H2_PAL_ERR_WOULD_BLOCK) {
            outcome = result;
            break;
        }
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    return outcome;
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
    uint32_t highest_newly_acknowledged = cumulative;
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
          if (h2_sctp_tsn_after(fragment->tsn, highest_newly_acknowledged)) {
            highest_newly_acknowledged = fragment->tsn;
          }
          h2_sctp_reliability_sample_rtt(association, fragment, now_ms);
          h2_sctp_reliability_mark_acknowledged(association, fragment,
                                                &acked_bytes);
        }
    }
    /* RFC 9260 section 7.2.4: fast recovery ends once the cumulative point
     * passes every TSN that was outstanding when it began. */
    if (association->fast_recovery_active &&
        !h2_sctp_tsn_before(cumulative, association->fast_recovery_exit_tsn)) {
      association->fast_recovery_active = false;
    }
    /* RFC 9260 section 7.2.4 HTNA: a repeated gap ACK is not new loss
     * evidence. Determine HTNA before counting holes, since fair stream
     * scheduling need not store fragments in TSN order. */
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL; fragment = fragment->next) {
      if (fragment->sent && !fragment->acknowledged && !fragment->abandoned &&
          h2_sctp_tsn_before(fragment->tsn, highest_newly_acknowledged)) {
        fragment->miss_reports++;
        if (fragment->miss_reports >= 3u) {
          fragment->fast_retransmit = true;
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
        if (association->rtt_sample_pending && fragment->tsn_assigned &&
            fragment->tsn == association->rtt_sample_tsn) {
          association->rtt_sample_pending = false;
        }
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
    /* RFC 9260 section 7.2.3: the timer expiry is one loss event, however
     * many fragments it covers, so the window collapse and the RTO backoff
     * are applied once per pass and not once per fragment. */
    bool window_collapsed = false;
    bool rto_backed_off = false;
    /* Every fragment is timed against the RTO in force when the pass began,
     * so the backoff applied to the first expiry cannot hide the others. */
    const uint64_t rto_at_entry = association->rto_ms;
    /* A fast retransmit that could not be emitted yet is retried here. Its
     * loss event has already been answered, so it must never fall through to
     * the timer response. */
    const h2_pal_result_t fast = h2_sctp_reliability_fast_retransmit(
        association, now_ms);
    if (fast != H2_PAL_OK && fast != H2_PAL_ERR_WOULD_BLOCK &&
        fast != H2_PAL_ERR_NO_MEMORY) {
        return fast;
    }
    if (fast != H2_PAL_OK) {
        deadline = now_ms;
    }
    for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (!fragment->sent || fragment->acknowledged || fragment->abandoned) {
            continue;
        }
        if (fragment->fast_retransmit) {
            deadline = now_ms;
            continue;
        }
        const uint64_t retransmit_deadline = h2_sctp_deadline_add(
            fragment->sent_ms, rto_at_entry);
        if (now_ms < retransmit_deadline) {
            if (deadline == H2_PAL_SCTP_NO_DEADLINE ||
                retransmit_deadline < deadline) {
                deadline = retransmit_deadline;
            }
            continue;
        }
        if (h2_sctp_reliability_outlived(fragment, now_ms)) {
            const uint64_t message_id = fragment->message_id;
            h2_sctp_reliability_abandon_message(association, message_id);
            continue;
        }
        if (association->pending_emit != NULL) {
            deadline = now_ms;
            break;
        }
        if (!window_collapsed) {
            association->ssthresh =
                h2_sctp_reliability_reduced_ssthresh(association);
            association->cwnd = h2_sctp_reliability_one_packet(association);
            /* A timer expiry supersedes any fast recovery in progress. */
            association->fast_recovery_active = false;
            window_collapsed = true;
        }
        h2_pal_result_t result = h2_sctp_reliability_emit_fragment(
            association, fragment, now_ms, true);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
            return result;
        }
        if (!rto_backed_off) {
            if (association->rto_ms < H2_SCTP_RTO_MAX_MS / 2u) {
                association->rto_ms *= 2u;
            } else {
                association->rto_ms = H2_SCTP_RTO_MAX_MS;
            }
            rto_backed_off = true;
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
