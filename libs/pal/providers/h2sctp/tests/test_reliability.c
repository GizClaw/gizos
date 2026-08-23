#include "h2_sctp_internal.h"
#include "h2_sctp_reliability.h"
#include "h2_sctp_test_peer.h"
#include "h2_sctp_wire.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static h2_pal_sctp_message_t make_message(
    const uint8_t *data,
    size_t len,
    uint16_t stream,
    h2_pal_sctp_reliability_t reliability,
    uint32_t value) {
    const h2_pal_sctp_message_t message = {
        .data = data,
        .len = len,
        .stream_id = stream,
        .ppid = 53u,
        .reliability = reliability,
        .reliability_value = value,
    };
    return message;
}

static void assert_sack_rejected_without_mutation(
    h2_pal_sctp_association_t *association,
    uint8_t *data,
    size_t len) {
    const size_t send_used = association->send_used;
    const size_t flight_size = association->flight_size;
    const uint32_t peer_cumulative = association->peer_cumulative_tsn;
    const uint32_t peer_window = association->peer_receive_window;
    h2_sctp_tx_fragment_t *const fragment = association->tx_fragments;
    uint8_t *const fragment_data = fragment->data;
    const h2_sctp_chunk_view_t sack = {
        .type = H2_SCTP_CHUNK_SACK,
        .data = data,
        .len = len,
    };
    assert(h2_sctp_reliability_handle_sack(
               association, &sack, 1u) == H2_PAL_ERR_FORMAT);
    assert(association->send_used == send_used);
    assert(association->flight_size == flight_size);
    assert(association->peer_cumulative_tsn == peer_cumulative);
    assert(association->peer_receive_window == peer_window);
    assert(association->tx_fragments == fragment);
    assert(fragment->data == fragment_data);
    assert(!fragment->acknowledged);
}

static h2_pal_result_t transfer_one(
    h2_sctp_test_endpoint_t *from,
    h2_sctp_test_endpoint_t *to,
    uint64_t now_ms) {
    assert(from->packet_head != NULL);
    h2_sctp_test_packet_t *packet = from->packet_head;
    const h2_pal_result_t result = h2_pal_sctp_association_input_packet(
        to->api, to->association, packet->data, packet->len, now_ms);
    if (result == H2_PAL_OK) {
        from->packet_head = packet->next;
        if (from->packet_head == NULL) {
            from->packet_tail = NULL;
        }
        free(packet->data);
        free(packet);
    }
    return result;
}

static size_t receive_fragment_count(
    const h2_pal_sctp_association_t *association) {
    size_t count = 0u;
    for (const h2_sctp_rx_fragment_t *fragment = association->rx_fragments;
         fragment != NULL;
         fragment = fragment->next) {
        count++;
    }
    return count;
}

int main(void) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    bool writable = false;
    assert(h2_pal_sctp_association_is_writable(
               pair.active.api, pair.active.association, &writable) ==
           H2_PAL_OK);
    assert(writable);
    const size_t saved_flight_size = pair.active.association->flight_size;
    pair.active.association->flight_size = pair.active.association->cwnd;
    assert(h2_pal_sctp_association_is_writable(
               pair.active.api, pair.active.association, &writable) ==
           H2_PAL_OK);
    assert(!writable);
    pair.active.association->flight_size = saved_flight_size;
    uint8_t first[100];
    memset(first, 0x11, sizeof(first));
    h2_pal_sctp_message_t message = make_message(
        first,
        sizeof(first),
        1u,
        H2_PAL_SCTP_RELIABILITY_RELIABLE,
        0u);
    pair.active.drop_next = 1u;
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_OK);

    uint8_t impossible_cumulative[12] = {0};
    h2_sctp_wire_write_u32(
        impossible_cumulative, pair.active.association->next_tsn);
    h2_sctp_wire_write_u32(impossible_cumulative + 4u, 123u);
    assert_sack_rejected_without_mutation(
        pair.active.association,
        impossible_cumulative,
        sizeof(impossible_cumulative));

    uint8_t invalid_gap[16] = {0};
    h2_sctp_wire_write_u32(
        invalid_gap, pair.active.association->peer_cumulative_tsn);
    h2_sctp_wire_write_u32(invalid_gap + 4u, 123u);
    h2_sctp_wire_write_u16(invalid_gap + 8u, 1u);
    h2_sctp_wire_write_u16(invalid_gap + 12u, 0u);
    h2_sctp_wire_write_u16(invalid_gap + 14u, 1u);
    assert_sack_rejected_without_mutation(
        pair.active.association, invalid_gap, sizeof(invalid_gap));

    uint8_t overlapping_gaps[20] = {0};
    h2_sctp_wire_write_u32(
        overlapping_gaps, pair.active.association->peer_cumulative_tsn);
    h2_sctp_wire_write_u32(overlapping_gaps + 4u, 123u);
    h2_sctp_wire_write_u16(overlapping_gaps + 8u, 2u);
    h2_sctp_wire_write_u16(overlapping_gaps + 12u, 1u);
    h2_sctp_wire_write_u16(overlapping_gaps + 14u, 1u);
    h2_sctp_wire_write_u16(overlapping_gaps + 16u, 1u);
    h2_sctp_wire_write_u16(overlapping_gaps + 18u, 1u);
    assert_sack_rejected_without_mutation(
        pair.active.association,
        overlapping_gaps,
        sizeof(overlapping_gaps));

    uint8_t trailing_sack_data[16] = {0};
    h2_sctp_wire_write_u32(
        trailing_sack_data, pair.active.association->peer_cumulative_tsn);
    h2_sctp_wire_write_u32(trailing_sack_data + 4u, 123u);
    trailing_sack_data[12] = 0xa5u;
    assert_sack_rejected_without_mutation(
        pair.active.association,
        trailing_sack_data,
        sizeof(trailing_sack_data));

    (void)h2_sctp_test_pump(&pair, 8u);
    assert(pair.passive.message_count == 0u);
    const uint32_t initial_cwnd = pair.active.association->cwnd;
    pair.now_ms += 1000u;
    uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
    assert(h2_pal_sctp_association_service(
               pair.active.api,
               pair.active.association,
               pair.now_ms,
               &deadline) == H2_PAL_OK);
    (void)h2_sctp_test_pump(&pair, 16u);
    assert(pair.passive.message_count == 1u);
    assert(pair.active.association->cwnd <= initial_cwnd);

    uint8_t second[80];
    memset(second, 0x22, sizeof(second));
    message = make_message(
        second,
        sizeof(second),
        2u,
        H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS,
        0u);
    pair.active.drop_next = 1u;
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_OK);
    (void)h2_sctp_test_pump(&pair, 8u);
    pair.now_ms += pair.active.association->rto_ms;
    assert(h2_pal_sctp_association_service(
               pair.active.api,
               pair.active.association,
               pair.now_ms,
               &deadline) == H2_PAL_OK);
    (void)h2_sctp_test_pump(&pair, 16u);
    assert(pair.passive.message_count == 1u);

    uint8_t third[120];
    memset(third, 0x33, sizeof(third));
    message = make_message(
        third,
        sizeof(third),
        3u,
        H2_PAL_SCTP_RELIABILITY_RELIABLE,
        0u);
    pair.passive.fail_allocation_at = pair.passive.allocation_count + 1u;
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_OK);
    (void)h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms);
    assert(pair.active.packet_head != NULL);
    assert(pair.passive.message_count == 1u);
    pair.passive.fail_allocation_at = 0u;
    pair.active.duplicate_next = 1u;
    (void)h2_sctp_test_pump(&pair, 16u);
    assert(pair.passive.message_count == 2u);
    assert(pair.passive.messages[1].len == sizeof(third));
    assert(memcmp(pair.passive.messages[1].data, third, sizeof(third)) == 0);
    h2_sctp_test_pair_deinit(&pair);

    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    uint8_t fragmented[300];
    memset(fragmented, 0x44, sizeof(fragmented));
    message = make_message(
        fragmented,
        sizeof(fragmented),
        4u,
        H2_PAL_SCTP_RELIABILITY_RELIABLE,
        0u);
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_OK);
    assert(transfer_one(
               &pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.message_count == 0u);
    assert(receive_fragment_count(pair.passive.association) == 1u);
    const size_t receive_used = pair.passive.association->receive_used;
    const uint32_t cumulative =
        pair.passive.association->cumulative_received_tsn;
    pair.passive.fail_allocation_at =
        pair.passive.allocation_count + 3u;
    assert(transfer_one(
               &pair.active,
               &pair.passive,
               pair.now_ms) == H2_PAL_ERR_WOULD_BLOCK);
    assert(pair.passive.allocation_failure_count == 1u);
    assert(pair.passive.message_count == 0u);
    assert(pair.passive.association->receive_used == receive_used);
    assert(pair.passive.association->cumulative_received_tsn == cumulative);
    assert(receive_fragment_count(pair.passive.association) == 1u);
    pair.passive.fail_allocation_at = 0u;
    assert(transfer_one(
               &pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.message_count == 1u);
    assert(pair.passive.messages[0].len == sizeof(fragmented));
    assert(memcmp(
               pair.passive.messages[0].data,
               fragmented,
               sizeof(fragmented)) == 0);
    (void)h2_sctp_test_pump(&pair, 16u);
    assert(pair.passive.message_count == 1u);

    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_OK);
    assert(transfer_one(
               &pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.packet_head == NULL);
    assert(transfer_one(
               &pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.packet_head != NULL);
    assert(pair.passive.association->sack_pending_packets == 0u);
    (void)h2_sctp_test_pump(&pair, 16u);
    assert(pair.passive.message_count == 2u);
    h2_sctp_test_pair_deinit(&pair);

    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    uint8_t callback_blocked[100];
    memset(callback_blocked, 0x66, sizeof(callback_blocked));
    message = make_message(
        callback_blocked,
        sizeof(callback_blocked),
        6u,
        H2_PAL_SCTP_RELIABILITY_RELIABLE,
        0u);
    pair.passive.message_would_block_count = 1u;
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_OK);
    assert(transfer_one(
               &pair.active,
               &pair.passive,
               pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.message_count == 0u);
    assert(pair.passive.packet_head != NULL);
    assert(pair.active.packet_head == NULL);
    assert(pair.passive.association->delivery_pending);
    deadline = H2_PAL_SCTP_NO_DEADLINE;
    assert(h2_pal_sctp_association_service(
               pair.passive.api,
               pair.passive.association,
               pair.now_ms,
               &deadline) == H2_PAL_OK);
    assert(pair.passive.message_count == 1u);
    assert(pair.passive.messages[0].len == sizeof(callback_blocked));
    assert(memcmp(
               pair.passive.messages[0].data,
               callback_blocked,
               sizeof(callback_blocked)) == 0);
    assert(pair.passive.packet_head != NULL);
    h2_sctp_test_pair_deinit(&pair);

    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    uint8_t delayed_ack[600];
    memset(delayed_ack, 0x55, sizeof(delayed_ack));
    message = make_message(
        delayed_ack,
        sizeof(delayed_ack),
        5u,
        H2_PAL_SCTP_RELIABILITY_RELIABLE,
        0u);
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_OK);
    assert(transfer_one(
               &pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.packet_head == NULL);
    assert(pair.passive.association->sack_pending_packets == 1u);
    assert(pair.passive.association->sack_deadline_ms ==
           pair.now_ms + H2_SCTP_DELAYED_SACK_MS);
    deadline = H2_PAL_SCTP_NO_DEADLINE;
    assert(h2_pal_sctp_association_service(
               pair.passive.api,
               pair.passive.association,
               pair.now_ms + H2_SCTP_DELAYED_SACK_MS - 1u,
               &deadline) == H2_PAL_OK);
    assert(pair.passive.packet_head == NULL);
    assert(deadline == pair.now_ms + H2_SCTP_DELAYED_SACK_MS);
    pair.now_ms += H2_SCTP_DELAYED_SACK_MS;
    deadline = H2_PAL_SCTP_NO_DEADLINE;
    assert(h2_pal_sctp_association_service(
               pair.passive.api,
               pair.passive.association,
               pair.now_ms,
               &deadline) == H2_PAL_OK);
    assert(pair.passive.packet_head != NULL);
    assert(pair.passive.association->sack_pending_packets == 0u);
    (void)h2_sctp_test_pump(&pair, 16u);
    assert(pair.passive.message_count == 1u);
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_OK);
    assert(transfer_one(
               &pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.packet_head == NULL);
    assert(transfer_one(
               &pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.packet_head != NULL);
    assert(pair.passive.association->sack_pending_packets == 0u);
    (void)h2_sctp_test_pump(&pair, 16u);
    assert(pair.passive.message_count == 2u);
    h2_sctp_test_pair_deinit(&pair);
    return 0;
}
