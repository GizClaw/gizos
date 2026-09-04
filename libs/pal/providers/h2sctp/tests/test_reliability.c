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
    const uint64_t rto = association->rto_ms;
    const bool rtt_pending = association->rtt_sample_pending;
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
    assert(association->rto_ms == rto);
    assert(association->rtt_sample_pending == rtt_pending);
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

static void acknowledge_at(h2_pal_sctp_association_t *association, uint32_t tsn,
                           uint64_t now_ms) {
  uint8_t body[12] = {0};
  h2_sctp_wire_write_u32(body, tsn);
  h2_sctp_wire_write_u32(body + 4u, 65536u);
  const h2_sctp_chunk_view_t sack = {
      .type = H2_SCTP_CHUNK_SACK,
      .data = body,
      .len = sizeof(body),
  };
  assert(h2_sctp_reliability_handle_sack(association, &sack, now_ms) ==
         H2_PAL_OK);
}

static uint32_t send_sample(h2_sctp_test_pair_t *pair, uint64_t now_ms) {
  const uint8_t byte = 42u;
  const h2_pal_sctp_message_t message =
      make_message(&byte, 1u, 1u, H2_PAL_SCTP_RELIABILITY_RELIABLE, 0u);
  assert(h2_pal_sctp_association_send_message(pair->active.api,
                                              pair->active.association,
                                              &message, now_ms) == H2_PAL_OK);
  assert(pair->active.association->tx_fragments_tail->sent);
  return pair->active.association->tx_fragments_tail->tsn;
}

static void test_rto_recovers_after_loss(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  assert(association->rto_ms == H2_SCTP_RTO_INITIAL_MS);
  uint32_t tsn = send_sample(&pair, 100u);
  acknowledge_at(association, tsn, 200u);
  /* First RTT: SRTT=100, RTTVAR=50, RTO=300 milliseconds. */
  assert(association->rto_ms == 300u);
  tsn = send_sample(&pair, 300u);
  acknowledge_at(association, tsn, 500u);
  /* RTTVAR uses the previous SRTT, before updating SRTT. */
  assert(association->rto_ms == 360u);
  tsn = send_sample(&pair, 600u);
  assert(h2_sctp_reliability_service(association, 960u, NULL) == H2_PAL_OK);
  assert(association->tx_fragments->retransmits == 1u);
  assert(association->rto_ms == 720u);
  acknowledge_at(association, tsn, 1000u);
  /* An ambiguous ACK must not sample the retransmission (Karn). */
  assert(association->rto_ms == 720u);
  tsn = send_sample(&pair, 1100u);
  acknowledge_at(association, tsn, 1200u);
  assert(association->rto_ms == 306u);
  /* A duplicate ACK cannot supply another RTT observation. */
  acknowledge_at(association, tsn, 1300u);
  assert(association->rto_ms == 306u);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_rtt_sample_lifetime(bool wrap_tsn) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  if (wrap_tsn) {
    association->next_tsn = UINT32_MAX;
    association->peer_cumulative_tsn = UINT32_MAX - 1u;
    association->advanced_peer_ack = UINT32_MAX - 1u;
  }
  const uint32_t first = send_sample(&pair, 100u);
  (void)send_sample(&pair, 110u);
  assert(association->rtt_sample_tsn == first);
  acknowledge_at(association, first, 200u);
  assert(association->rto_ms == 300u);
  assert(!association->rtt_sample_pending);
  const uint32_t third = send_sample(&pair, 250u);
  assert(association->rtt_sample_tsn == third);
  /* An earlier TSN is retransmitted while the third TSN is sampled. */
  assert(h2_sctp_reliability_service(association, 410u, NULL) == H2_PAL_OK);
  assert(association->tx_fragments->retransmits == 1u);
  assert(!association->rtt_sample_pending);
  acknowledge_at(association, third, 500u);
  assert(association->rto_ms == 600u);
  const uint32_t fourth = send_sample(&pair, 600u);
  acknowledge_at(association, fourth, 700u);
  assert(association->rto_ms == 248u);
  /* Shutdown's cumulative ACK has no measurement timestamp. */
  const uint32_t fifth = send_sample(&pair, 800u);
  h2_sctp_reliability_acknowledge_through(association, fifth);
  assert(!association->rtt_sample_pending);
  assert(association->rto_ms == 248u);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_rtt_one_sample_per_flight(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  const uint32_t first = send_sample(&pair, 100u);
  const uint32_t second = send_sample(&pair, 110u);
  acknowledge_at(association, first, 200u);
  assert(association->rto_ms == 300u);
  acknowledge_at(association, second, 250u);
  assert(association->rto_ms == 300u);
  assert(!association->rtt_sample_pending);
  /* A gap ACK can complete the sampled TSN, without a second sample when
   * it is subsequently acknowledged cumulatively. */
  (void)send_sample(&pair, 300u);
  const uint32_t fourth = send_sample(&pair, 310u);
  uint8_t body[16] = {0};
  h2_sctp_wire_write_u32(body, second);
  h2_sctp_wire_write_u32(body + 4u, 65536u);
  h2_sctp_wire_write_u16(body + 8u, 1u);
  h2_sctp_wire_write_u16(body + 12u, 1u);
  h2_sctp_wire_write_u16(body + 14u, 1u);
  const h2_sctp_chunk_view_t sack = {
      .type = H2_SCTP_CHUNK_SACK,
      .data = body,
      .len = sizeof(body),
  };
  assert(h2_sctp_reliability_handle_sack(association, &sack, 400u) ==
         H2_PAL_OK);
  assert(association->rto_ms == 248u);
  acknowledge_at(association, fourth, 450u);
  assert(association->rto_ms == 248u);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_rtt_bounds_and_abandon(void) {
  const uint64_t samples[] = {0u, 1u, 100000u, UINT64_MAX - 1000u};
  for (size_t index = 0u; index < sizeof(samples) / sizeof(samples[0]);
       ++index) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    h2_pal_sctp_association_t *association = pair.active.association;
    const uint32_t tsn = send_sample(&pair, 100u);
    acknowledge_at(association, tsn, 100u + samples[index]);
    assert(association->rto_ms ==
           (index < 2u ? H2_SCTP_RTO_MIN_MS : H2_SCTP_RTO_MAX_MS));
    assert(association->rttvar_ms != 0u);
    if (index == 3u) {
      const uint32_t next = send_sample(&pair, UINT64_MAX - 500u);
      acknowledge_at(association, next, UINT64_MAX - 1u);
      assert(association->rto_ms == H2_SCTP_RTO_MAX_MS);
      assert(association->srtt_ms > UINT64_MAX / 2u);
    }
    h2_sctp_test_pair_deinit(&pair);
  }
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  uint32_t tsn = send_sample(&pair, 100u);
  acknowledge_at(association, tsn, 99u);
  assert(!association->rtt_initialized);
  assert(!association->rtt_sample_pending);
  const uint8_t byte = 1u;
  const h2_pal_sctp_message_t message =
      make_message(&byte, 1u, 1u, H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS, 0u);
  assert(h2_pal_sctp_association_send_message(pair.active.api, association,
                                              &message, 200u) == H2_PAL_OK);
  assert(association->rtt_sample_pending);
  assert(h2_sctp_reliability_service(association, 1200u, NULL) == H2_PAL_OK);
  assert(!association->rtt_sample_pending);
  assert(!association->rtt_initialized);
  tsn = send_sample(&pair, 1300u);
  acknowledge_at(association, tsn, 1400u);
  assert(association->rto_ms == 300u);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_rtt_skips_local_backpressure(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  pair.active.emit_would_block_count = 1u;
  uint32_t tsn = send_sample(&pair, 100u);
  assert(association->pending_emit != NULL);
  assert(!association->rtt_sample_pending);
  assert(h2_sctp_retry_pending_emit(association) == H2_PAL_OK);
  acknowledge_at(association, tsn, 200u);
  assert(!association->rtt_initialized);
  assert(association->rto_ms == H2_SCTP_RTO_INITIAL_MS);
  tsn = send_sample(&pair, 300u);
  acknowledge_at(association, tsn, 400u);
  assert(association->rto_ms == 300u);
  h2_sctp_test_pair_deinit(&pair);
}

static void acknowledge_gap_at(h2_pal_sctp_association_t *association,
                               uint32_t cumulative, uint16_t start,
                               uint16_t end, uint64_t now_ms) {
  uint8_t body[16] = {0};
  h2_sctp_wire_write_u32(body, cumulative);
  h2_sctp_wire_write_u32(body + 4u, 65536u);
  h2_sctp_wire_write_u16(body + 8u, 1u);
  h2_sctp_wire_write_u16(body + 12u, start);
  h2_sctp_wire_write_u16(body + 14u, end);
  const h2_sctp_chunk_view_t sack = {
      .type = H2_SCTP_CHUNK_SACK,
      .data = body,
      .len = sizeof(body),
  };
  assert(h2_sctp_reliability_handle_sack(association, &sack, now_ms) ==
         H2_PAL_OK);
}

static void test_sack_counts_only_new_ack_evidence(bool wrap_tsn) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  if (wrap_tsn) {
    association->next_tsn = UINT32_MAX - 2u;
    association->peer_cumulative_tsn = UINT32_MAX - 3u;
    association->advanced_peer_ack = UINT32_MAX - 3u;
  }
  const uint32_t cumulative = association->peer_cumulative_tsn;
  for (unsigned index = 0u; index < 5u; ++index) {
    (void)send_sample(&pair, 1000u + index);
  }
  h2_sctp_tx_fragment_t *first = association->tx_fragments;
  h2_sctp_tx_fragment_t *second = first->next;
  h2_sctp_tx_fragment_t *fourth = second->next->next;
  acknowledge_gap_at(association, cumulative, 3u, 3u, 1100u);
  assert(first->miss_reports == 1u);
  assert(second->miss_reports == 1u);
  const uint32_t cwnd = association->cwnd;
  for (unsigned index = 0u; index < 8u; ++index) {
    acknowledge_gap_at(association, cumulative, 3u, 3u, 1101u + index);
    assert(first->miss_reports == 1u);
    assert(second->miss_reports == 1u);
    assert(first->retransmits == 0u);
    assert(association->cwnd == cwnd);
    assert(association->rto_ms == H2_SCTP_RTO_INITIAL_MS);
  }
  /* The highest NEW acknowledgment can be lower than an already known one. */
  acknowledge_gap_at(association, cumulative, 2u, 3u, 1120u);
  assert(first->miss_reports == 2u);
  assert(second->acknowledged);
  assert(fourth->miss_reports == 0u);
  acknowledge_gap_at(association, cumulative, 2u, 5u, 1130u);
  assert(first->retransmits == 1u);
  /* Still no new evidence after the retransmission. */
  acknowledge_gap_at(association, cumulative, 2u, 5u, 1140u);
  assert(first->miss_reports == 0u);
  assert(first->retransmits == 1u);
  acknowledge_at(association, cumulative + 5u, 1150u);
  assert(association->tx_fragments == NULL);
  assert(association->flight_size == 0u);
  h2_sctp_test_pair_deinit(&pair);
}

/*
 * A high TSN acknowledged by an earlier SACK must not act as HTNA again when
 * that SACK is repeated: replaying it supplies no new loss evidence, so holes
 * below it must not accrue further miss reports and trigger a fast retransmit.
 */
static void test_repeated_gap_ack_does_not_reuse_acked_tsn_as_htna(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  const uint32_t cumulative = association->peer_cumulative_tsn;
  for (unsigned index = 0u; index < 5u; ++index) {
    (void)send_sample(&pair, 1000u + index);
  }
  h2_sctp_tx_fragment_t *first = association->tx_fragments;
  h2_sctp_tx_fragment_t *second = first->next;
  h2_sctp_tx_fragment_t *third = second->next;
  h2_sctp_tx_fragment_t *fourth = third->next;

  /* Acknowledge only TSN cumulative+4, leaving three holes beneath it. */
  acknowledge_gap_at(association, cumulative, 4u, 4u, 1100u);
  assert(fourth->acknowledged);
  assert(first->miss_reports == 1u && second->miss_reports == 1u &&
         third->miss_reports == 1u);

  /* Replaying the same SACK is not new evidence: cumulative+4 is already
   * acknowledged, so HTNA falls back to the cumulative point and nothing
   * below it may be counted again or fast retransmitted. */
  for (unsigned round = 0u; round < 4u; ++round) {
    acknowledge_gap_at(association, cumulative, 4u, 4u, 1110u + round);
    assert(first->miss_reports == 1u);
    assert(second->miss_reports == 1u);
    assert(third->miss_reports == 1u);
    assert(first->retransmits == 0u && second->retransmits == 0u &&
           third->retransmits == 0u);
  }
  h2_sctp_test_pair_deinit(&pair);
}

/*
 * RFC 9260 section 7.2.4: the third miss report for a hole is a fast
 * retransmit, not a timer expiry. The window drops to the reduced ssthresh
 * rather than to a single MTU, and the RTO is not backed off.
 */
static void test_fast_retransmit_halves_window_without_rto_backoff(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  const uint32_t cumulative = association->peer_cumulative_tsn;
  for (unsigned index = 0u; index < 5u; ++index) {
    (void)send_sample(&pair, 1000u + index);
  }
  h2_sctp_tx_fragment_t *hole = association->tx_fragments;
  assert(hole->tsn == cumulative + 1u);

  /* Two miss reports are not yet loss evidence. */
  acknowledge_gap_at(association, cumulative, 2u, 2u, 1100u);
  acknowledge_gap_at(association, cumulative, 2u, 3u, 1110u);
  assert(hole->miss_reports == 2u);
  assert(hole->retransmits == 0u);
  assert(!association->fast_recovery_active);
  assert(association->rto_ms == H2_SCTP_RTO_INITIAL_MS);

  /* 8192 + one byte of slow-start growth halves to 4096, above 4 * MTU. */
  association->cwnd = 8192u;
  acknowledge_gap_at(association, cumulative, 2u, 4u, 1120u);
  assert(hole->retransmits == 1u);
  assert(!hole->fast_retransmit);
  assert(association->ssthresh == 4096u);
  assert(association->cwnd == association->ssthresh);
  assert(association->rto_ms == H2_SCTP_RTO_INITIAL_MS);
  assert(association->fast_recovery_active);
  assert(association->fast_recovery_exit_tsn == cumulative + 5u);
  h2_sctp_test_pair_deinit(&pair);
}

/*
 * RFC 9260 section 7.2.4: while fast recovery is in progress, further gap
 * reports for the same hole retransmit but must not reduce the window a
 * second time within that round trip.
 */
static void test_fast_retransmit_reduces_once_per_round_trip(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  const uint32_t cumulative = association->peer_cumulative_tsn;
  for (unsigned index = 0u; index < 9u; ++index) {
    (void)send_sample(&pair, 1000u + index);
  }
  h2_sctp_tx_fragment_t *hole = association->tx_fragments;
  assert(hole->tsn == cumulative + 1u);

  acknowledge_gap_at(association, cumulative, 2u, 2u, 1100u);
  acknowledge_gap_at(association, cumulative, 2u, 3u, 1110u);
  association->cwnd = 8192u;
  acknowledge_gap_at(association, cumulative, 2u, 4u, 1120u);
  assert(hole->retransmits == 1u);
  assert(association->ssthresh == 4096u);
  assert(association->cwnd == 4096u);
  assert(association->fast_recovery_active);

  /* The retransmission is lost too: three more miss reports mark the same
   * hole again inside the same recovery episode. */
  acknowledge_gap_at(association, cumulative, 2u, 5u, 1130u);
  acknowledge_gap_at(association, cumulative, 2u, 6u, 1140u);
  assert(hole->miss_reports == 2u);
  assert(hole->retransmits == 1u);
  acknowledge_gap_at(association, cumulative, 2u, 7u, 1150u);
  assert(hole->retransmits == 2u);
  /* Retransmitted, but neither the window nor the RTO reduced again. */
  assert(association->ssthresh == 4096u);
  assert(association->cwnd >= 4096u);
  assert(association->rto_ms == H2_SCTP_RTO_INITIAL_MS);
  assert(association->fast_recovery_active);

  /* Recovery ends once the cumulative point passes the outstanding TSNs. */
  acknowledge_at(association, cumulative + 9u, 1200u);
  assert(!association->fast_recovery_active);
  assert(association->tx_fragments == NULL);
  h2_sctp_test_pair_deinit(&pair);
}

/*
 * RFC 9260 section 7.2.3: a timer expiry covering several fragments is one
 * loss event, so the window collapses and the RTO doubles exactly once.
 */
static void test_retransmission_timeout_applies_once_per_burst(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  for (unsigned index = 0u; index < 4u; ++index) {
    (void)send_sample(&pair, 1000u);
  }
  association->cwnd = 8192u;
  assert(association->rto_ms == H2_SCTP_RTO_INITIAL_MS);
  assert(h2_sctp_reliability_service(
             association, 1000u + H2_SCTP_RTO_INITIAL_MS, NULL) == H2_PAL_OK);
  unsigned retransmitted = 0u;
  for (const h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
       fragment != NULL; fragment = fragment->next) {
    assert(fragment->retransmits == 1u);
    retransmitted++;
  }
  assert(retransmitted == 4u);
  /* One halving of 8192, one doubling of the RTO, and one MTU of window. */
  assert(association->ssthresh == 4096u);
  assert(association->cwnd == 256u);
  assert(association->rto_ms == 2u * H2_SCTP_RTO_INITIAL_MS);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_sack_htna_with_interleaved_streams(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  const uint32_t cumulative = association->peer_cumulative_tsn;
  association->peer_receive_window = 0u;
  const uint8_t byte = 42u;
  for (uint16_t stream = 5u; stream != 0u; --stream) {
    const h2_pal_sctp_message_t message =
        make_message(&byte, 1u, stream, H2_PAL_SCTP_RELIABILITY_RELIABLE, 0u);
    assert(h2_pal_sctp_association_send_message(pair.active.api, association,
                                                &message, 1000u) == H2_PAL_OK);
  }
  assert(association->flight_size == 0u);
  association->peer_receive_window = 65536u;
  assert(h2_sctp_reliability_send_pending(association, 1001u) == H2_PAL_OK);
  h2_sctp_tx_fragment_t *fragments[5] = {0};
  for (h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
       fragment != NULL; fragment = fragment->next) {
    const uint32_t offset = fragment->tsn - cumulative - 1u;
    assert(offset < 5u);
    fragments[offset] = fragment;
  }
  assert(association->tx_fragments == fragments[4]);
  acknowledge_gap_at(association, cumulative, 5u, 5u, 1100u);
  for (unsigned index = 0u; index < 4u; ++index) {
    assert(fragments[index]->miss_reports == 1u);
  }
  uint8_t body[20] = {0};
  h2_sctp_wire_write_u32(body, cumulative);
  h2_sctp_wire_write_u32(body + 4u, 65536u);
  h2_sctp_wire_write_u16(body + 8u, 2u);
  h2_sctp_wire_write_u16(body + 12u, 2u);
  h2_sctp_wire_write_u16(body + 14u, 2u);
  h2_sctp_wire_write_u16(body + 16u, 5u);
  h2_sctp_wire_write_u16(body + 18u, 5u);
  const h2_sctp_chunk_view_t sack = {
      .type = H2_SCTP_CHUNK_SACK,
      .data = body,
      .len = sizeof(body),
  };
  assert(h2_sctp_reliability_handle_sack(association, &sack, 1120u) ==
         H2_PAL_OK);
  assert(fragments[0]->miss_reports == 2u);
  assert(fragments[1]->acknowledged);
  assert(fragments[2]->miss_reports == 1u);
  assert(fragments[3]->miss_reports == 1u);
  acknowledge_at(association, cumulative + 5u, 1140u);
  assert(association->tx_fragments == NULL);
  assert(association->flight_size == 0u);
  h2_sctp_test_pair_deinit(&pair);
}

int main(void) {
  test_sack_counts_only_new_ack_evidence(false);
  test_sack_counts_only_new_ack_evidence(true);
  test_repeated_gap_ack_does_not_reuse_acked_tsn_as_htna();
  test_sack_htna_with_interleaved_streams();
  test_fast_retransmit_halves_window_without_rto_backoff();
  test_fast_retransmit_reduces_once_per_round_trip();
  test_retransmission_timeout_applies_once_per_burst();
  test_rto_recovers_after_loss();
  test_rtt_sample_lifetime(false);
  test_rtt_sample_lifetime(true);
  test_rtt_one_sample_per_flight();
  test_rtt_bounds_and_abandon();
  test_rtt_skips_local_backpressure();
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  bool writable = false;
  assert(h2_pal_sctp_association_is_writable(
             pair.active.api, pair.active.association, &writable) == H2_PAL_OK);
  assert(writable);
  const size_t saved_flight_size = pair.active.association->flight_size;
  pair.active.association->flight_size = pair.active.association->cwnd;
  assert(h2_pal_sctp_association_is_writable(
             pair.active.api, pair.active.association, &writable) == H2_PAL_OK);
  assert(!writable);
  pair.active.association->flight_size = saved_flight_size;
  uint8_t first[100];
  memset(first, 0x11, sizeof(first));
  h2_pal_sctp_message_t message = make_message(
      first, sizeof(first), 1u, H2_PAL_SCTP_RELIABILITY_RELIABLE, 0u);
  pair.active.drop_next = 1u;
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);

  uint8_t impossible_cumulative[12] = {0};
  h2_sctp_wire_write_u32(impossible_cumulative,
                         pair.active.association->next_tsn);
  h2_sctp_wire_write_u32(impossible_cumulative + 4u, 123u);
  assert_sack_rejected_without_mutation(pair.active.association,
                                        impossible_cumulative,
                                        sizeof(impossible_cumulative));

  uint8_t invalid_gap[16] = {0};
  h2_sctp_wire_write_u32(invalid_gap,
                         pair.active.association->peer_cumulative_tsn);
  h2_sctp_wire_write_u32(invalid_gap + 4u, 123u);
  h2_sctp_wire_write_u16(invalid_gap + 8u, 1u);
  h2_sctp_wire_write_u16(invalid_gap + 12u, 0u);
  h2_sctp_wire_write_u16(invalid_gap + 14u, 1u);
  assert_sack_rejected_without_mutation(pair.active.association, invalid_gap,
                                        sizeof(invalid_gap));

  uint8_t overlapping_gaps[20] = {0};
  h2_sctp_wire_write_u32(overlapping_gaps,
                         pair.active.association->peer_cumulative_tsn);
  h2_sctp_wire_write_u32(overlapping_gaps + 4u, 123u);
  h2_sctp_wire_write_u16(overlapping_gaps + 8u, 2u);
  h2_sctp_wire_write_u16(overlapping_gaps + 12u, 1u);
  h2_sctp_wire_write_u16(overlapping_gaps + 14u, 1u);
  h2_sctp_wire_write_u16(overlapping_gaps + 16u, 1u);
  h2_sctp_wire_write_u16(overlapping_gaps + 18u, 1u);
  assert_sack_rejected_without_mutation(
      pair.active.association, overlapping_gaps, sizeof(overlapping_gaps));

  uint8_t trailing_sack_data[16] = {0};
  h2_sctp_wire_write_u32(trailing_sack_data,
                         pair.active.association->peer_cumulative_tsn);
  h2_sctp_wire_write_u32(trailing_sack_data + 4u, 123u);
  trailing_sack_data[12] = 0xa5u;
  assert_sack_rejected_without_mutation(
      pair.active.association, trailing_sack_data, sizeof(trailing_sack_data));

  (void)h2_sctp_test_pump(&pair, 8u);
  assert(pair.passive.message_count == 0u);
  const uint32_t initial_cwnd = pair.active.association->cwnd;
  pair.now_ms += 1000u;
  uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
  assert(h2_pal_sctp_association_service(pair.active.api,
                                         pair.active.association, pair.now_ms,
                                         &deadline) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 16u);
  assert(pair.passive.message_count == 1u);
  assert(pair.active.association->cwnd <= initial_cwnd);

  uint8_t second[80];
  memset(second, 0x22, sizeof(second));
  message = make_message(second, sizeof(second), 2u,
                         H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS, 0u);
  pair.active.drop_next = 1u;
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 8u);
  pair.now_ms += pair.active.association->rto_ms;
  assert(h2_pal_sctp_association_service(pair.active.api,
                                         pair.active.association, pair.now_ms,
                                         &deadline) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 16u);
  assert(pair.passive.message_count == 1u);

  uint8_t third[120];
  memset(third, 0x33, sizeof(third));
  message = make_message(third, sizeof(third), 3u,
                         H2_PAL_SCTP_RELIABILITY_RELIABLE, 0u);
  pair.passive.fail_allocation_at = pair.passive.allocation_count + 1u;
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
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
  message = make_message(fragmented, sizeof(fragmented), 4u,
                         H2_PAL_SCTP_RELIABILITY_RELIABLE, 0u);
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.message_count == 0u);
  assert(receive_fragment_count(pair.passive.association) == 1u);
  const size_t receive_used = pair.passive.association->receive_used;
  const uint32_t cumulative = pair.passive.association->cumulative_received_tsn;
  pair.passive.fail_allocation_at = pair.passive.allocation_count + 3u;
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(pair.passive.allocation_failure_count == 1u);
  assert(pair.passive.message_count == 0u);
  assert(pair.passive.association->receive_used == receive_used);
  assert(pair.passive.association->cumulative_received_tsn == cumulative);
  assert(receive_fragment_count(pair.passive.association) == 1u);
  pair.passive.fail_allocation_at = 0u;
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.message_count == 1u);
  assert(pair.passive.messages[0].len == sizeof(fragmented));
  assert(memcmp(pair.passive.messages[0].data, fragmented,
                sizeof(fragmented)) == 0);
  (void)h2_sctp_test_pump(&pair, 16u);
  assert(pair.passive.message_count == 1u);

  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.packet_head == NULL);
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.packet_head != NULL);
  assert(pair.passive.association->sack_pending_packets == 0u);
  (void)h2_sctp_test_pump(&pair, 16u);
  assert(pair.passive.message_count == 2u);
  h2_sctp_test_pair_deinit(&pair);

  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  uint8_t callback_blocked[100];
  memset(callback_blocked, 0x66, sizeof(callback_blocked));
  message = make_message(callback_blocked, sizeof(callback_blocked), 6u,
                         H2_PAL_SCTP_RELIABILITY_RELIABLE, 0u);
  pair.passive.message_would_block_count = 1u;
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.message_count == 0u);
  assert(pair.passive.packet_head != NULL);
  assert(pair.active.packet_head == NULL);
  assert(pair.passive.association->delivery_pending);
  deadline = H2_PAL_SCTP_NO_DEADLINE;
  assert(h2_pal_sctp_association_service(pair.passive.api,
                                         pair.passive.association, pair.now_ms,
                                         &deadline) == H2_PAL_OK);
  assert(pair.passive.message_count == 1u);
  assert(pair.passive.messages[0].len == sizeof(callback_blocked));
  assert(memcmp(pair.passive.messages[0].data, callback_blocked,
                sizeof(callback_blocked)) == 0);
  assert(pair.passive.packet_head != NULL);
  h2_sctp_test_pair_deinit(&pair);

  assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  uint8_t delayed_ack[600];
  memset(delayed_ack, 0x55, sizeof(delayed_ack));
  message = make_message(delayed_ack, sizeof(delayed_ack), 5u,
                         H2_PAL_SCTP_RELIABILITY_RELIABLE, 0u);
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.packet_head == NULL);
  assert(pair.passive.association->sack_pending_packets == 1u);
  assert(pair.passive.association->sack_deadline_ms ==
         pair.now_ms + H2_SCTP_DELAYED_SACK_MS);
  deadline = H2_PAL_SCTP_NO_DEADLINE;
  assert(h2_pal_sctp_association_service(
             pair.passive.api, pair.passive.association,
             pair.now_ms + H2_SCTP_DELAYED_SACK_MS - 1u,
             &deadline) == H2_PAL_OK);
  assert(pair.passive.packet_head == NULL);
  assert(deadline == pair.now_ms + H2_SCTP_DELAYED_SACK_MS);
  pair.now_ms += H2_SCTP_DELAYED_SACK_MS;
  deadline = H2_PAL_SCTP_NO_DEADLINE;
  assert(h2_pal_sctp_association_service(pair.passive.api,
                                         pair.passive.association, pair.now_ms,
                                         &deadline) == H2_PAL_OK);
  assert(pair.passive.packet_head != NULL);
  assert(pair.passive.association->sack_pending_packets == 0u);
  (void)h2_sctp_test_pump(&pair, 16u);
  assert(pair.passive.message_count == 1u);
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.packet_head == NULL);
  assert(transfer_one(&pair.active, &pair.passive, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.packet_head != NULL);
  assert(pair.passive.association->sack_pending_packets == 0u);
  (void)h2_sctp_test_pump(&pair, 16u);
  assert(pair.passive.message_count == 2u);
  h2_sctp_test_pair_deinit(&pair);
  return 0;
}
