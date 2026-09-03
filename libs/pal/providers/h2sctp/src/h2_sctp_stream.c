#include "h2_sctp_stream.h"

#include "h2_sctp_reliability.h"

#include <limits.h>
#include <string.h>

h2_sctp_stream_t *h2_sctp_stream_find(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id) {
    for (h2_sctp_stream_t *stream = association->streams;
         stream != NULL;
         stream = stream->next) {
        if (stream->id == stream_id) {
            return stream;
        }
    }
    return NULL;
}

h2_sctp_stream_t *h2_sctp_stream_get_or_create(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id) {
    h2_sctp_stream_t *stream = h2_sctp_stream_find(association, stream_id);
    if (stream != NULL) {
        return stream;
    }
    stream = h2_sctp_alloc(association->owner, sizeof(*stream));
    if (stream == NULL) {
        return NULL;
    }
    stream->id = stream_id;
    stream->next = association->streams;
    association->streams = stream;
    return stream;
}

static void h2_sctp_stream_free_tx_list(
    h2_pal_sctp_association_t *association,
    h2_sctp_tx_fragment_t *fragments) {
    while (fragments != NULL) {
        h2_sctp_tx_fragment_t *next = fragments->next;
        h2_sctp_free(association->owner, fragments->data);
        h2_sctp_free(association->owner, fragments);
        fragments = next;
    }
}

h2_pal_result_t h2_sctp_stream_queue_message(
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_message_t *message,
    uint64_t now_ms) {
    if (message->len > association->config.send_buffer_size ||
        association->send_used > association->config.send_buffer_size -
                                     message->len) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    const size_t data_header_size = association->peer_interleaving ? 20u : 16u;
    if (association->config.max_packet_size <=
        H2_SCTP_COMMON_HEADER_SIZE + data_header_size) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const size_t payload_size = association->config.max_packet_size -
                                H2_SCTP_COMMON_HEADER_SIZE -
                                data_header_size;
    const size_t fragment_count =
        message->len / payload_size + (message->len % payload_size != 0u);
    if (fragment_count == 0u || fragment_count > UINT32_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_sctp_stream_t *stream = h2_sctp_stream_get_or_create(
        association, message->stream_id);
    if (stream == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const uint32_t identifier = association->peer_interleaving
                                    ? (message->unordered
                                           ? stream->next_out_mid_unordered
                                           : stream->next_out_mid_ordered)
                                    : stream->next_out_ssn;
    h2_sctp_tx_fragment_t *head = NULL;
    h2_sctp_tx_fragment_t *last = NULL;
    h2_sctp_tx_fragment_t **tail = &head;
    size_t offset = 0u;
    for (size_t index = 0u; index < fragment_count; ++index) {
        const size_t remaining = message->len - offset;
        const size_t fragment_len =
            remaining < payload_size ? remaining : payload_size;
        h2_sctp_tx_fragment_t *fragment = h2_sctp_alloc(
            association->owner, sizeof(*fragment));
        if (fragment == NULL) {
            h2_sctp_stream_free_tx_list(association, head);
            return H2_PAL_ERR_NO_MEMORY;
        }
        fragment->data = h2_sctp_alloc(association->owner, fragment_len);
        if (fragment->data == NULL) {
            h2_sctp_free(association->owner, fragment);
            h2_sctp_stream_free_tx_list(association, head);
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(fragment->data, message->data + offset, fragment_len);
        fragment->message_id = association->next_message_id;
        fragment->message_identifier = identifier;
        fragment->fragment_sequence = (uint32_t)index;
        fragment->ppid = message->ppid;
        fragment->stream_id = message->stream_id;
        fragment->flags = message->unordered
                              ? H2_SCTP_DATA_FLAG_UNORDERED
                              : 0u;
        if (index == 0u) {
            fragment->flags |= H2_SCTP_DATA_FLAG_BEGIN;
        }
        if (index + 1u == fragment_count) {
            fragment->flags |= H2_SCTP_DATA_FLAG_END;
        }
        fragment->reliability = message->reliability;
        fragment->reliability_value = message->reliability_value;
        fragment->submitted_ms = now_ms;
        fragment->data_len = fragment_len;
        *tail = fragment;
        tail = &fragment->next;
        if (last != NULL) {
            last->stream_next = fragment;
        }
        last = fragment;
        offset += fragment_len;
    }

    if (association->peer_interleaving) {
        if (message->unordered) {
            stream->next_out_mid_unordered++;
        } else {
            stream->next_out_mid_ordered++;
        }
    } else {
        stream->next_out_ssn++;
    }
    association->next_message_id++;
    association->send_used += message->len;
    if (association->tx_fragments_tail == NULL) {
        association->tx_fragments = head;
    } else {
        association->tx_fragments_tail->next = head;
    }
    association->tx_fragments_tail = last;
    if (stream->tx_tail != NULL) {
        stream->tx_tail->stream_next = head;
    }
    if (stream->tx_unsent == NULL) {
        stream->tx_unsent = head;
    }
    stream->tx_tail = last;
    return H2_PAL_OK;
}

static h2_sctp_rx_fragment_t *h2_sctp_stream_find_rx_tsn(
    h2_pal_sctp_association_t *association,
    uint32_t tsn) {
    for (h2_sctp_rx_fragment_t *fragment = association->rx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        if (fragment->tsn == tsn) {
            return fragment;
        }
    }
    return NULL;
}

static void h2_sctp_stream_insert_rx(
    h2_pal_sctp_association_t *association,
    h2_sctp_rx_fragment_t *fragment) {
    fragment->next = NULL;
    if (association->rx_fragments_tail == NULL) {
        association->rx_fragments = fragment;
        association->rx_fragments_tail = fragment;
        return;
    }
    if (h2_sctp_tsn_before(
            association->rx_fragments_tail->tsn, fragment->tsn)) {
        association->rx_fragments_tail->next = fragment;
        association->rx_fragments_tail = fragment;
        return;
    }
    h2_sctp_rx_fragment_t **cursor = &association->rx_fragments;
    while (*cursor != NULL && h2_sctp_tsn_before((*cursor)->tsn, fragment->tsn)) {
        cursor = &(*cursor)->next;
    }
    fragment->next = *cursor;
    *cursor = fragment;
}

static void h2_sctp_stream_remove_rx(
    h2_pal_sctp_association_t *association,
    h2_sctp_rx_fragment_t *fragment) {
    h2_sctp_rx_fragment_t **cursor = &association->rx_fragments;
    h2_sctp_rx_fragment_t *previous = NULL;
    while (*cursor != NULL && *cursor != fragment) {
        previous = *cursor;
        cursor = &(*cursor)->next;
    }
    if (*cursor == fragment) {
        *cursor = fragment->next;
        if (association->rx_fragments_tail == fragment) {
            association->rx_fragments_tail = previous;
        }
        fragment->next = NULL;
    }
}

static void h2_sctp_stream_advance_cumulative(
    h2_pal_sctp_association_t *association,
    h2_sctp_rx_fragment_t *fragment) {
    if (fragment->tsn != association->cumulative_received_tsn + 1u) {
        return;
    }
    for (h2_sctp_rx_fragment_t *cursor = fragment;
         cursor != NULL &&
         cursor->tsn == association->cumulative_received_tsn + 1u;
         cursor = cursor->next) {
        association->cumulative_received_tsn = cursor->tsn;
    }
}

static bool h2_sctp_stream_same_message(
    const h2_sctp_rx_fragment_t *fragment,
    uint16_t stream_id,
    uint32_t identifier,
    bool unordered,
    bool interleaved) {
    return fragment->stream_id == stream_id &&
           fragment->message_identifier == identifier &&
           ((fragment->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u) ==
               unordered &&
           fragment->interleaved == interleaved && !fragment->delivered;
}

static h2_sctp_rx_fragment_t *h2_sctp_stream_find_message_fragment(
    h2_sctp_rx_fragment_t *first,
    uint16_t stream_id,
    uint32_t identifier,
    bool unordered,
    bool interleaved,
    uint32_t fragment_sequence,
    uint32_t expected_tsn) {
    for (h2_sctp_rx_fragment_t *fragment = first;
         fragment != NULL;
         fragment = fragment->next) {
        if (!h2_sctp_stream_same_message(
                fragment, stream_id, identifier, unordered, interleaved)) {
            continue;
        }
        if ((interleaved &&
             fragment->fragment_sequence == fragment_sequence) ||
            (!interleaved && fragment->tsn == expected_tsn)) {
            return fragment;
        }
    }
    return NULL;
}

static bool h2_sctp_stream_message_ready(
    h2_pal_sctp_association_t *association,
    h2_sctp_rx_fragment_t *begin,
    size_t *out_size,
    uint32_t *out_last_sequence) {
    size_t total = 0u;
    uint32_t sequence = 0u;
    uint32_t expected_tsn = begin->tsn;
    h2_sctp_rx_fragment_t *cursor = begin;
    for (;;) {
        h2_sctp_rx_fragment_t *fragment = h2_sctp_stream_find_message_fragment(
            cursor,
            begin->stream_id,
            begin->message_identifier,
            (begin->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u,
            begin->interleaved,
            sequence,
            expected_tsn);
        if (fragment == NULL || fragment->data_len > SIZE_MAX - total) {
            return false;
        }
        total += fragment->data_len;
        if (total > association->config.max_message_size) {
            return false;
        }
        if ((fragment->flags & H2_SCTP_DATA_FLAG_END) != 0u) {
            *out_size = total;
            *out_last_sequence = sequence;
            return true;
        }
        cursor = fragment->next;
        sequence++;
        expected_tsn++;
    }
}

static void h2_sctp_stream_prune_delivered(
    h2_pal_sctp_association_t *association) {
    h2_sctp_rx_fragment_t **cursor = &association->rx_fragments;
    h2_sctp_rx_fragment_t *tail = NULL;
    while (*cursor != NULL) {
        h2_sctp_rx_fragment_t *fragment = *cursor;
        if (fragment->delivered &&
            !h2_sctp_tsn_after(
                fragment->tsn, association->cumulative_received_tsn)) {
            *cursor = fragment->next;
            h2_sctp_free(association->owner, fragment);
            continue;
        }
        tail = fragment;
        cursor = &fragment->next;
    }
    association->rx_fragments_tail = tail;
}

static void h2_sctp_stream_refresh_rx_tail(
    h2_pal_sctp_association_t *association) {
    association->rx_fragments_tail = NULL;
    for (h2_sctp_rx_fragment_t *fragment = association->rx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        association->rx_fragments_tail = fragment;
    }
}

static h2_pal_result_t h2_sctp_stream_deliver_one(
    h2_pal_sctp_association_t *association,
    h2_sctp_rx_fragment_t *begin,
    size_t total,
    uint32_t last_sequence,
    uint8_t *message_data) {
    if (message_data == NULL) {
        message_data = h2_sctp_alloc(association->owner, total);
    }
    if (message_data == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    size_t offset = 0u;
    h2_sctp_rx_fragment_t *cursor = begin;
    for (uint32_t sequence = 0u; sequence <= last_sequence; ++sequence) {
        h2_sctp_rx_fragment_t *fragment = h2_sctp_stream_find_message_fragment(
            cursor,
            begin->stream_id,
            begin->message_identifier,
            (begin->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u,
            begin->interleaved,
            sequence,
            begin->tsn + sequence);
        memcpy(message_data + offset, fragment->data, fragment->data_len);
        offset += fragment->data_len;
        cursor = fragment->next;
    }
    const h2_pal_sctp_received_message_t message = {
        .data = message_data,
        .len = total,
        .stream_id = begin->stream_id,
        .ppid = begin->ppid,
        .unordered =
            (begin->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u,
    };
    const h2_pal_result_t delivery_result =
        h2_sctp_notify_message(association, &message);
    h2_sctp_free(association->owner, message_data);
    if (delivery_result != H2_PAL_OK) {
        return delivery_result;
    }

    cursor = begin;
    for (uint32_t sequence = 0u; sequence <= last_sequence; ++sequence) {
        h2_sctp_rx_fragment_t *fragment = h2_sctp_stream_find_message_fragment(
            cursor,
            begin->stream_id,
            begin->message_identifier,
            message.unordered,
            begin->interleaved,
            sequence,
            begin->tsn + sequence);
        cursor = fragment->next;
        association->receive_used -= fragment->data_len;
        h2_sctp_free(association->owner, fragment->data);
        fragment->data = NULL;
        fragment->data_len = 0u;
        fragment->delivered = true;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_sctp_stream_deliver_ready(
    h2_pal_sctp_association_t *association) {
    bool delivered;
    do {
        delivered = false;
        for (h2_sctp_rx_fragment_t *fragment = association->rx_fragments;
             fragment != NULL;
             fragment = fragment->next) {
            if (fragment->delivered ||
                (fragment->flags & H2_SCTP_DATA_FLAG_BEGIN) == 0u) {
                continue;
            }
            const bool unordered =
                (fragment->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u;
            h2_sctp_stream_t *stream = h2_sctp_stream_get_or_create(
                association, fragment->stream_id);
            if (stream == NULL) {
                return H2_PAL_ERR_NO_MEMORY;
            }
            const uint32_t expected = fragment->interleaved
                                          ? stream->next_in_mid_ordered
                                          : stream->next_in_ssn;
            if (!unordered && fragment->message_identifier != expected) {
                continue;
            }
            size_t total = 0u;
            uint32_t last_sequence = 0u;
            if (!h2_sctp_stream_message_ready(
                    association, fragment, &total, &last_sequence)) {
                continue;
            }
            h2_pal_result_t result = h2_sctp_stream_deliver_one(
                association, fragment, total, last_sequence, NULL);
            if (result != H2_PAL_OK) {
                return result;
            }
            if (!unordered) {
                if (fragment->interleaved) {
                    stream->next_in_mid_ordered++;
                } else {
                    stream->next_in_ssn++;
                }
            }
            delivered = true;
            break;
        }
    } while (delivered);
    h2_sctp_stream_prune_delivered(association);
    return H2_PAL_OK;
}

h2_pal_result_t h2_sctp_stream_service(
    h2_pal_sctp_association_t *association) {
    if (!association->delivery_pending) {
        return H2_PAL_OK;
    }
    const h2_pal_result_t result = h2_sctp_stream_deliver_ready(association);
    association->delivery_pending = result != H2_PAL_OK;
    return result;
}

h2_pal_result_t h2_sctp_stream_handle_data(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    const bool interleaved = chunk->type == H2_SCTP_CHUNK_I_DATA;
    const size_t header_size = interleaved ? 16u : 12u;
    if (interleaved != association->peer_interleaving ||
        chunk->len <= header_size ||
        (chunk->flags & ~(H2_SCTP_DATA_FLAG_IMMEDIATE |
                          H2_SCTP_DATA_FLAG_UNORDERED |
                          H2_SCTP_DATA_FLAG_BEGIN |
                          H2_SCTP_DATA_FLAG_END)) != 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    const uint32_t tsn = h2_sctp_wire_read_u32(chunk->data);
    const uint16_t stream_id = h2_sctp_wire_read_u16(chunk->data + 4u);
    if (stream_id >= association->negotiated_inbound_streams) {
        return H2_PAL_ERR_FORMAT;
    }
    const bool in_order = tsn == association->cumulative_received_tsn + 1u;
    if (!h2_sctp_tsn_after(tsn, association->cumulative_received_tsn) ||
        (!in_order && h2_sctp_stream_find_rx_tsn(association, tsn) != NULL)) {
        h2_pal_result_t result = h2_sctp_reliability_send_sack(
            association, now_ms);
        if (result == H2_PAL_ERR_NO_MEMORY) {
            return H2_PAL_OK;
        }
        return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
    }

    const size_t payload_len = chunk->len - header_size;
    if (payload_len > association->config.receive_buffer_size ||
        association->receive_used > association->config.receive_buffer_size -
                                        payload_len) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    h2_sctp_rx_fragment_t *fragment = h2_sctp_alloc(
        association->owner, sizeof(*fragment));
    if (fragment == NULL) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    fragment->data = h2_sctp_alloc(association->owner, payload_len);
    if (fragment->data == NULL) {
        h2_sctp_free(association->owner, fragment);
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    memcpy(fragment->data, chunk->data + header_size, payload_len);
    fragment->tsn = tsn;
    fragment->stream_id = stream_id;
    fragment->flags = chunk->flags;
    fragment->interleaved = interleaved;
    fragment->data_len = payload_len;
    if (interleaved) {
        if (h2_sctp_wire_read_u16(chunk->data + 6u) != 0u) {
            h2_sctp_free(association->owner, fragment->data);
            h2_sctp_free(association->owner, fragment);
            return H2_PAL_ERR_FORMAT;
        }
        fragment->message_identifier = h2_sctp_wire_read_u32(
            chunk->data + 8u);
        if ((chunk->flags & H2_SCTP_DATA_FLAG_BEGIN) != 0u) {
            fragment->ppid = h2_sctp_wire_read_u32(chunk->data + 12u);
            fragment->fragment_sequence = 0u;
        } else {
            fragment->fragment_sequence = h2_sctp_wire_read_u32(
                chunk->data + 12u);
        }
    } else {
        fragment->message_identifier = h2_sctp_wire_read_u16(
            chunk->data + 6u);
        fragment->ppid = h2_sctp_wire_read_u32(chunk->data + 8u);
    }

    h2_sctp_stream_insert_rx(association, fragment);
    h2_sctp_rx_fragment_t *begin =
        (fragment->flags & H2_SCTP_DATA_FLAG_BEGIN) != 0u ? fragment : NULL;
    if (begin == NULL) {
        for (h2_sctp_rx_fragment_t *candidate = association->rx_fragments;
             candidate != NULL;
             candidate = candidate->next) {
            if ((candidate->flags & H2_SCTP_DATA_FLAG_BEGIN) != 0u &&
                h2_sctp_stream_same_message(
                    candidate,
                    fragment->stream_id,
                    fragment->message_identifier,
                    (fragment->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u,
                    fragment->interleaved)) {
                begin = candidate;
                break;
            }
        }
    }

    h2_sctp_stream_t *delivery_stream = NULL;
    size_t delivery_size = 0u;
    uint32_t last_sequence = 0u;
    uint8_t *delivery_data = NULL;
    if (begin != NULL && h2_sctp_stream_message_ready(
                             association,
                             begin,
                             &delivery_size,
                             &last_sequence)) {
        const bool unordered =
            (begin->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u;
        delivery_stream = h2_sctp_stream_find(
            association, begin->stream_id);
        const uint32_t expected =
            delivery_stream == NULL
                ? 0u
                : (begin->interleaved
                       ? delivery_stream->next_in_mid_ordered
                       : delivery_stream->next_in_ssn);
        if (unordered || begin->message_identifier == expected) {
            delivery_data = h2_sctp_alloc(
                association->owner, delivery_size);
            if (delivery_data == NULL) {
                h2_sctp_stream_remove_rx(association, fragment);
                h2_sctp_free(association->owner, fragment->data);
                h2_sctp_free(association->owner, fragment);
                return H2_PAL_ERR_WOULD_BLOCK;
            }
            if (!unordered && delivery_stream == NULL) {
                delivery_stream = h2_sctp_stream_get_or_create(
                    association, begin->stream_id);
                if (delivery_stream == NULL) {
                    h2_sctp_free(association->owner, delivery_data);
                    h2_sctp_stream_remove_rx(association, fragment);
                    h2_sctp_free(association->owner, fragment->data);
                    h2_sctp_free(association->owner, fragment);
                    return H2_PAL_ERR_WOULD_BLOCK;
                }
            }
        }
    }

    association->receive_used += payload_len;
    h2_sctp_stream_advance_cumulative(association, fragment);
    bool delivery_blocked = false;
    if (delivery_data != NULL) {
        const bool unordered =
            (begin->flags & H2_SCTP_DATA_FLAG_UNORDERED) != 0u;
        h2_pal_result_t result = h2_sctp_stream_deliver_one(
            association,
            begin,
            delivery_size,
            last_sequence,
            delivery_data);
        if (result == H2_PAL_ERR_WOULD_BLOCK) {
            association->delivery_pending = true;
            delivery_blocked = true;
        } else if (result != H2_PAL_OK) {
            return result;
        } else if (!unordered) {
            if (begin->interleaved) {
                delivery_stream->next_in_mid_ordered++;
            } else {
                delivery_stream->next_in_ssn++;
            }
        }
        if (!delivery_blocked) {
            h2_sctp_stream_prune_delivered(association);
            association->delivery_pending = true;
        }
    }
    const bool immediate_sack =
        !in_order ||
        (chunk->flags & (H2_SCTP_DATA_FLAG_IMMEDIATE |
                         H2_SCTP_DATA_FLAG_END)) != 0u;
    h2_pal_result_t result = h2_sctp_reliability_note_data(
        association, now_ms, immediate_sack);
    if (result == H2_PAL_ERR_NO_MEMORY) {
        return H2_PAL_OK;
    }
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    return H2_PAL_OK;
}

static void h2_sctp_stream_discard_through(
    h2_pal_sctp_association_t *association,
    uint32_t cumulative_tsn) {
    h2_sctp_rx_fragment_t **cursor = &association->rx_fragments;
    while (*cursor != NULL) {
        h2_sctp_rx_fragment_t *fragment = *cursor;
        if (!h2_sctp_tsn_after(fragment->tsn, cumulative_tsn)) {
            *cursor = fragment->next;
            association->receive_used -= fragment->data_len;
            h2_sctp_free(association->owner, fragment->data);
            h2_sctp_free(association->owner, fragment);
            continue;
        }
        cursor = &fragment->next;
    }
    h2_sctp_stream_refresh_rx_tail(association);
}

static h2_sctp_stream_t *h2_sctp_stream_find_prepared(
    h2_sctp_stream_t *streams,
    uint16_t stream_id) {
    for (h2_sctp_stream_t *stream = streams;
         stream != NULL;
         stream = stream->next) {
        if (stream->id == stream_id) {
            return stream;
        }
    }
    return NULL;
}

static h2_pal_result_t h2_sctp_stream_prepare_forward_streams(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    size_t entry_size,
    bool interleaved) {
    h2_sctp_stream_t *prepared = NULL;
    for (size_t offset = 4u; offset < chunk->len; offset += entry_size) {
        const uint16_t stream_id = h2_sctp_wire_read_u16(
            chunk->data + offset);
        if (stream_id >= association->negotiated_inbound_streams ||
            (interleaved &&
             (h2_sctp_wire_read_u16(chunk->data + offset + 2u) & ~1u) !=
                 0u)) {
            while (prepared != NULL) {
                h2_sctp_stream_t *next = prepared->next;
                h2_sctp_free(association->owner, prepared);
                prepared = next;
            }
            return H2_PAL_ERR_FORMAT;
        }
        if (h2_sctp_stream_find(association, stream_id) != NULL ||
            h2_sctp_stream_find_prepared(prepared, stream_id) != NULL) {
            continue;
        }
        h2_sctp_stream_t *stream = h2_sctp_alloc(
            association->owner, sizeof(*stream));
        if (stream == NULL) {
            while (prepared != NULL) {
                h2_sctp_stream_t *next = prepared->next;
                h2_sctp_free(association->owner, prepared);
                prepared = next;
            }
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        stream->id = stream_id;
        stream->next = prepared;
        prepared = stream;
    }
    while (prepared != NULL) {
        h2_sctp_stream_t *next = prepared->next;
        prepared->next = association->streams;
        association->streams = prepared;
        prepared = next;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_sctp_stream_handle_forward_tsn(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    const bool interleaved = chunk->type == H2_SCTP_CHUNK_I_FORWARD_TSN;
    const size_t entry_size = interleaved ? 8u : 4u;
    if (interleaved != association->peer_interleaving || chunk->len < 4u ||
        (chunk->len - 4u) % entry_size != 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    const uint32_t new_cumulative = h2_sctp_wire_read_u32(chunk->data);
    if (!h2_sctp_tsn_after(
            new_cumulative, association->cumulative_received_tsn)) {
        h2_pal_result_t result = h2_sctp_reliability_send_sack(
            association, now_ms);
        if (result == H2_PAL_ERR_NO_MEMORY ||
            result == H2_PAL_ERR_WOULD_BLOCK) {
            return H2_PAL_OK;
        }
        return result;
    }
    h2_pal_result_t result = h2_sctp_stream_prepare_forward_streams(
        association, chunk, entry_size, interleaved);
    if (result != H2_PAL_OK) {
        return result;
    }
    association->cumulative_received_tsn = new_cumulative;
    h2_sctp_stream_discard_through(association, new_cumulative);
    for (size_t offset = 4u; offset < chunk->len; offset += entry_size) {
        const uint16_t stream_id = h2_sctp_wire_read_u16(chunk->data + offset);
        h2_sctp_stream_t *stream = h2_sctp_stream_find(
            association, stream_id);
        if (interleaved) {
            const uint16_t flags = h2_sctp_wire_read_u16(
                chunk->data + offset + 2u);
            const uint32_t mid = h2_sctp_wire_read_u32(
                chunk->data + offset + 4u);
            if ((flags & 1u) != 0u) {
                if (!h2_sctp_tsn_after(stream->next_in_mid_unordered, mid)) {
                    stream->next_in_mid_unordered = mid + 1u;
                }
            } else if (!h2_sctp_tsn_after(stream->next_in_mid_ordered, mid)) {
                stream->next_in_mid_ordered = mid + 1u;
            }
        } else {
            const uint16_t ssn = h2_sctp_wire_read_u16(
                chunk->data + offset + 2u);
            if ((uint16_t)(ssn + 1u - stream->next_in_ssn) < UINT16_MAX / 2u) {
                stream->next_in_ssn = (uint16_t)(ssn + 1u);
            }
        }
    }
    association->delivery_pending = true;
    result = h2_sctp_stream_service(association);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_NO_MEMORY) {
        return result;
    }
    result = h2_sctp_reliability_send_sack(association, now_ms);
    if (result == H2_PAL_ERR_NO_MEMORY) {
        return H2_PAL_OK;
    }
    return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : result;
}

static void h2_sctp_stream_reset_incoming(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id) {
    h2_sctp_stream_t *stream = h2_sctp_stream_find(association, stream_id);
    if (stream != NULL) {
        stream->next_in_ssn = 0u;
        stream->next_in_mid_ordered = 0u;
        stream->next_in_mid_unordered = 0u;
    }
    h2_sctp_rx_fragment_t **cursor = &association->rx_fragments;
    while (*cursor != NULL) {
        h2_sctp_rx_fragment_t *fragment = *cursor;
        if (fragment->stream_id == stream_id) {
            *cursor = fragment->next;
            association->receive_used -= fragment->data_len;
            h2_sctp_free(association->owner, fragment->data);
            h2_sctp_free(association->owner, fragment);
            continue;
        }
        cursor = &fragment->next;
    }
    h2_sctp_stream_refresh_rx_tail(association);
    h2_sctp_notify_stream_reset(
        association,
        stream_id,
        H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET,
        H2_PAL_OK);
}

static h2_pal_result_t h2_sctp_stream_send_reset_response(
    h2_pal_sctp_association_t *association,
    uint32_t request_sequence,
    uint32_t result_code,
    uint64_t now_ms) {
    uint8_t chunk[16] = {0};
    chunk[0] = H2_SCTP_CHUNK_RE_CONFIG;
    h2_sctp_wire_write_u16(chunk + 2u, sizeof(chunk));
    h2_sctp_wire_write_u16(chunk + 4u, H2_SCTP_PARAM_RESET_RESPONSE);
    h2_sctp_wire_write_u16(chunk + 6u, 12u);
    h2_sctp_wire_write_u32(chunk + 8u, request_sequence);
    h2_sctp_wire_write_u32(chunk + 12u, result_code);
    return h2_sctp_emit_chunks(
        association,
        association->peer_verification_tag,
        chunk,
        sizeof(chunk),
        H2_SCTP_CONTROL_NONE,
        now_ms);
}

h2_pal_result_t h2_sctp_stream_handle_reconfig(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms) {
    size_t offset = 0u;
    while (offset < chunk->len) {
        if (chunk->len - offset < 4u) {
            return H2_PAL_ERR_TRUNCATED;
        }
        const uint16_t type = h2_sctp_wire_read_u16(chunk->data + offset);
        const size_t length = h2_sctp_wire_read_u16(chunk->data + offset + 2u);
        if (length < 4u || length > chunk->len - offset) {
            return H2_PAL_ERR_FORMAT;
        }
        if (type == H2_SCTP_PARAM_OUTGOING_RESET) {
            if (length < 18u || (length - 16u) % 2u != 0u) {
                return H2_PAL_ERR_FORMAT;
            }
            const uint32_t sequence = h2_sctp_wire_read_u32(
                chunk->data + offset + 4u);
            if (association->expected_reset_sequence == 0u) {
                association->expected_reset_sequence = sequence;
            }
            uint32_t result_code = 1u;
            if (h2_sctp_tsn_before(
                    sequence, association->expected_reset_sequence)) {
                result_code = 0u;
            } else if (sequence != association->expected_reset_sequence) {
                result_code = 5u;
            } else {
                for (size_t stream_offset = 16u;
                     stream_offset < length;
                     stream_offset += 2u) {
                    const uint16_t stream_id = h2_sctp_wire_read_u16(
                        chunk->data + offset + stream_offset);
                    if (stream_id >= association->negotiated_inbound_streams) {
                        result_code = 2u;
                        break;
                    }
                }
            }
            h2_pal_result_t emit_result = h2_sctp_stream_send_reset_response(
                association, sequence, result_code, now_ms);
            if (emit_result == H2_PAL_ERR_NO_MEMORY) {
                return H2_PAL_ERR_WOULD_BLOCK;
            }
            if (emit_result != H2_PAL_OK &&
                emit_result != H2_PAL_ERR_WOULD_BLOCK) {
                return emit_result;
            }
            if (result_code == 1u) {
                for (size_t stream_offset = 16u;
                     stream_offset < length;
                     stream_offset += 2u) {
                    h2_sctp_stream_reset_incoming(
                        association,
                        h2_sctp_wire_read_u16(
                            chunk->data + offset + stream_offset));
                }
                association->expected_reset_sequence++;
            }
        } else if (type == H2_SCTP_PARAM_RESET_RESPONSE) {
            if (length != 12u && length != 20u) {
                return H2_PAL_ERR_FORMAT;
            }
            const uint32_t sequence = h2_sctp_wire_read_u32(
                chunk->data + offset + 4u);
            const uint32_t result_code = h2_sctp_wire_read_u32(
                chunk->data + offset + 8u);
            for (h2_sctp_stream_t *stream = association->streams;
                 stream != NULL;
                 stream = stream->next) {
                if (!stream->reset_pending ||
                    stream->reset_request_sequence != sequence) {
                    continue;
                }
                stream->reset_pending = false;
                h2_sctp_clear_control(association);
                if (result_code == 0u || result_code == 1u) {
                    stream->next_out_ssn = 0u;
                    stream->next_out_mid_ordered = 0u;
                    stream->next_out_mid_unordered = 0u;
                    h2_sctp_notify_stream_reset(
                        association,
                        stream->id,
                        H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED,
                        H2_PAL_OK);
                } else {
                    h2_sctp_notify_stream_reset(
                        association,
                        stream->id,
                        H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED,
                        H2_PAL_ERR_INVALID_STATE);
                }
                break;
            }
        }
        if (length > SIZE_MAX - 3u) {
            return H2_PAL_ERR_FORMAT;
        }
        offset += (length + 3u) & ~(size_t)3u;
    }
    return H2_PAL_OK;
}

void h2_sctp_stream_release_all(h2_pal_sctp_association_t *association) {
    h2_sctp_stream_t *stream = association->streams;
    while (stream != NULL) {
        h2_sctp_stream_t *next = stream->next;
        h2_sctp_free(association->owner, stream);
        stream = next;
    }
    association->streams = NULL;
    h2_sctp_rx_fragment_t *fragment = association->rx_fragments;
    while (fragment != NULL) {
        h2_sctp_rx_fragment_t *next = fragment->next;
        h2_sctp_free(association->owner, fragment->data);
        h2_sctp_free(association->owner, fragment);
        fragment = next;
    }
    association->rx_fragments = NULL;
    association->rx_fragments_tail = NULL;
    association->receive_used = 0u;
}
