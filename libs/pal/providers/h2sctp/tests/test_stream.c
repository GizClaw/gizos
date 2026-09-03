#include "h2_sctp_internal.h"
#include "h2_sctp_stream.h"
#include "h2_sctp_test_peer.h"
#include "h2_sctp_wire.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static void fill_payload(uint8_t *data, size_t len, uint8_t seed) {
    for (size_t index = 0u; index < len; ++index) {
        data[index] = (uint8_t)(seed + index);
    }
}

static void send_message(
    h2_sctp_test_pair_t *pair,
    uint16_t stream_id,
    uint32_t ppid,
    bool unordered,
    const uint8_t *data,
    size_t len) {
    const h2_pal_sctp_message_t message = {
        .data = data,
        .len = len,
        .stream_id = stream_id,
        .ppid = ppid,
        .unordered = unordered,
        .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
        .reliability_value = 0u,
    };
    assert(h2_pal_sctp_association_send_message(
               pair->active.api,
               pair->active.association,
               &message,
               pair->now_ms) == H2_PAL_OK);
}

static h2_sctp_stream_t *find_stream(
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

static void assert_stale_forward_tsn_does_not_advance_stream(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    uint64_t now_ms) {
    h2_sctp_stream_t *const stream = find_stream(association, stream_id);
    assert(stream != NULL);
    const uint32_t cumulative = association->cumulative_received_tsn;
    uint8_t data[12] = {0};
    h2_sctp_wire_write_u32(data, cumulative);
    h2_sctp_wire_write_u16(data + 4u, stream_id);
    h2_sctp_chunk_view_t chunk = {
        .data = data,
    };
    uint32_t sequence = 0u;
    if (association->peer_interleaving) {
        chunk.type = H2_SCTP_CHUNK_I_FORWARD_TSN;
        chunk.len = sizeof(data);
        sequence = stream->next_in_mid_ordered;
        h2_sctp_wire_write_u32(data + 8u, sequence + 10u);
    } else {
        chunk.type = H2_SCTP_CHUNK_FORWARD_TSN;
        chunk.len = 8u;
        sequence = stream->next_in_ssn;
        h2_sctp_wire_write_u16(data + 6u, (uint16_t)(sequence + 10u));
    }
    assert(h2_sctp_stream_handle_forward_tsn(
               association, &chunk, now_ms) == H2_PAL_OK);
    assert(association->cumulative_received_tsn == cumulative);
    if (association->peer_interleaving) {
        assert(stream->next_in_mid_ordered == sequence);
    } else {
        assert(stream->next_in_ssn == (uint16_t)sequence);
    }
}

static void test_reset_in_progress(uint32_t success_code) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 4096u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_pal_sctp_association_t *association = pair.active.association;
  assert(h2_pal_sctp_association_reset_stream(pair.active.api, association, 2u,
                                              ++pair.now_ms) == H2_PAL_OK);
  h2_sctp_stream_t *stream = find_stream(association, 2u);
  assert(stream != NULL && stream->reset_pending);
  uint8_t response[12] = {0};
  h2_sctp_wire_write_u16(response, H2_SCTP_PARAM_RESET_RESPONSE);
  h2_sctp_wire_write_u16(response + 2u, sizeof(response));
  h2_sctp_wire_write_u32(response + 4u, stream->reset_request_sequence);
  h2_sctp_wire_write_u32(response + 8u, 6u);
  const h2_sctp_chunk_view_t chunk = {
      .type = H2_SCTP_CHUNK_RE_CONFIG,
      .data = response,
      .len = sizeof(response),
  };
  const uint64_t initial_deadline = association->control_deadline_ms;
  h2_sctp_wire_write_u32(response + 4u, stream->reset_request_sequence + 1u);
  assert(h2_sctp_stream_handle_reconfig(association, &chunk, ++pair.now_ms) ==
         H2_PAL_OK);
  assert(!association->control_reset_in_progress &&
         association->control_deadline_ms == initial_deadline);
  h2_sctp_wire_write_u32(response + 4u, stream->reset_request_sequence);
  assert(h2_sctp_stream_handle_reconfig(association, &chunk, ++pair.now_ms) ==
         H2_PAL_OK);
  assert(stream->reset_pending && pair.active.outgoing_reset_events == 0u);
  assert(association->control_packet != NULL);
  assert(association->control_deadline_ms == pair.now_ms + association->rto_ms);
  const size_t saved_len = association->control_packet_len;
  uint8_t saved_request[256];
  assert(saved_len <= sizeof(saved_request));
  memcpy(saved_request, association->control_packet, saved_len);
  // More deferred retries than the ordinary loss limit must stay alive.
  for (unsigned i = 0u; i <= H2_SCTP_MAX_CONTROL_RETRIES; ++i) {
    pair.now_ms = association->control_deadline_ms;
    const uint64_t old_rto = association->rto_ms;
    uint64_t deadline;
    assert(h2_pal_sctp_association_service(pair.active.api, association,
                                           pair.now_ms,
                                           &deadline) == H2_PAL_OK);
    assert(association->control_retries == 0u);
    assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
    assert(stream->reset_pending && pair.active.outgoing_reset_events == 0u);
    assert(association->rto_ms == (old_rto < H2_SCTP_RTO_MAX_MS / 2u
                                       ? old_rto * 2u
                                       : H2_SCTP_RTO_MAX_MS));
    unsigned resets = 0u;
    for (h2_sctp_test_packet_t *packet = pair.active.packet_head;
         packet != NULL; packet = packet->next) {
      if (packet->len == saved_len &&
          memcmp(packet->data, saved_request, saved_len) == 0)
        ++resets;
    }
    assert(resets == i + 2u);
  }
  h2_sctp_wire_write_u32(response + 8u, success_code);
  assert(h2_sctp_stream_handle_reconfig(association, &chunk, ++pair.now_ms) ==
         H2_PAL_OK);
  assert(!stream->reset_pending && pair.active.outgoing_reset_events == 1u);
  assert(association->control_packet == NULL);
  assert(!association->control_reset_in_progress);
  // A late duplicate must not resurrect the stopped timer or complete twice.
  h2_sctp_wire_write_u32(response + 8u, 6u);
  assert(h2_sctp_stream_handle_reconfig(association, &chunk, ++pair.now_ms) ==
         H2_PAL_OK);
  assert(!association->control_reset_in_progress &&
         association->control_deadline_ms == H2_PAL_SCTP_NO_DEADLINE &&
         pair.active.outgoing_reset_events == 1u);
  h2_sctp_test_pair_deinit(&pair);
}

int main(void) {
  test_reset_in_progress(0u);
  test_reset_in_progress(1u);
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 256u, 4096u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));

  uint8_t first[600];
  uint8_t second[700];
  uint8_t third[800];
  fill_payload(first, sizeof(first), 0x10u);
  fill_payload(second, sizeof(second), 0x20u);
  fill_payload(third, sizeof(third), 0x30u);
  pair.now_ms++;
  send_message(&pair, 1u, 51u, false, first, sizeof(first));
  send_message(&pair, 2u, 53u, true, second, sizeof(second));
  send_message(&pair, 3u, 53u, false, third, sizeof(third));
  (void)h2_sctp_test_pump(&pair, 128u);
  assert(pair.passive.message_count == 3u);
  assert(pair.passive.messages[0].stream_id == 1u);
  assert(pair.passive.messages[0].ppid == 51u);
  assert(pair.passive.messages[0].len == sizeof(first));
  assert(memcmp(pair.passive.messages[0].data, first, sizeof(first)) == 0);
  assert(pair.passive.messages[1].stream_id == 2u);
  assert(pair.passive.messages[1].unordered);
  assert(memcmp(pair.passive.messages[1].data, second, sizeof(second)) == 0);
  assert(pair.passive.messages[2].stream_id == 3u);
  assert(memcmp(pair.passive.messages[2].data, third, sizeof(third)) == 0);

  h2_pal_sctp_message_t reverse = {
      .data = third,
      .len = sizeof(third),
      .stream_id = 1u,
      .ppid = 53u,
      .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
  };
  assert(h2_pal_sctp_association_send_message(
             pair.passive.api, pair.passive.association, &reverse,
             pair.now_ms) == H2_PAL_OK);
  reverse.data = second;
  reverse.len = sizeof(second);
  reverse.stream_id = 2u;
  reverse.unordered = true;
  assert(h2_pal_sctp_association_send_message(
             pair.passive.api, pair.passive.association, &reverse,
             pair.now_ms) == H2_PAL_OK);
  reverse.data = first;
  reverse.len = sizeof(first);
  reverse.stream_id = 3u;
  reverse.unordered = false;
  assert(h2_pal_sctp_association_send_message(
             pair.passive.api, pair.passive.association, &reverse,
             pair.now_ms) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 128u);
  assert(pair.active.message_count == 3u);

  pair.now_ms++;
  assert(h2_pal_sctp_association_reset_stream(pair.active.api,
                                              pair.active.association, 2u,
                                              pair.now_ms) == H2_PAL_OK);
  assert(pair.active.packet_head != NULL);
  uint8_t delayed_reset[256];
  const size_t delayed_reset_len = pair.active.packet_head->len;
  assert(delayed_reset_len <= sizeof(delayed_reset));
  memcpy(delayed_reset, pair.active.packet_head->data, delayed_reset_len);
  (void)h2_sctp_test_pump(&pair, 32u);
  assert(pair.active.outgoing_reset_events == 1u);
  assert(pair.passive.incoming_reset_events == 1u);

  const uint8_t reopened[] = {0xdeu, 0xadu, 0xbeu, 0xefu};
  pair.now_ms++;
  send_message(&pair, 2u, 53u, false, reopened, sizeof(reopened));
  (void)h2_sctp_test_pump(&pair, 32u);
  assert(pair.passive.message_count == 4u);
  assert(pair.passive.messages[3].stream_id == 2u);
  assert(memcmp(pair.passive.messages[3].data, reopened, sizeof(reopened)) ==
         0);

  pair.now_ms++;
  assert(h2_pal_sctp_association_input_packet(
             pair.passive.api, pair.passive.association, delayed_reset,
             delayed_reset_len, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.incoming_reset_events == 1u);
  (void)h2_sctp_test_pump(&pair, 32u);
  assert(pair.active.outgoing_reset_events == 1u);
  const uint8_t reopened_again[] = {0x12u, 0x34u};
  pair.now_ms++;
  send_message(&pair, 2u, 53u, false, reopened_again, sizeof(reopened_again));
  (void)h2_sctp_test_pump(&pair, 32u);
  assert(pair.passive.message_count == 5u);
  assert(pair.passive.messages[4].stream_id == 2u);
  assert(memcmp(pair.passive.messages[4].data, reopened_again,
                sizeof(reopened_again)) == 0);
  assert_stale_forward_tsn_does_not_advance_stream(pair.passive.association, 2u,
                                                   pair.now_ms);

  h2_sctp_test_pair_deinit(&pair);

  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  pair.active.association->peer_interleaving = false;
  pair.passive.association->peer_interleaving = false;
  uint8_t fallback[16u * 1024u + 1u] = {0};
  h2_pal_sctp_message_t fallback_message = {
      .data = fallback,
      .len = sizeof(fallback) - 1u,
      .stream_id = 7u,
      .ppid = 53u,
      .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
  };
  assert(h2_pal_sctp_association_send_message(
             pair.active.api, pair.active.association, &fallback_message,
             pair.now_ms) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 128u);
  assert(pair.passive.message_count == 1u);
  assert(pair.passive.messages[0].len == sizeof(fallback) - 1u);
  assert_stale_forward_tsn_does_not_advance_stream(pair.passive.association, 7u,
                                                   pair.now_ms);
  const uint8_t abandoned = 0xa5u;
  fallback_message.data = &abandoned;
  fallback_message.len = 1u;
  fallback_message.stream_id = 8u;
  fallback_message.reliability = H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS;
  fallback_message.reliability_value = 0u;
  pair.active.drop_next = 1u;
  assert(h2_pal_sctp_association_send_message(
             pair.active.api, pair.active.association, &fallback_message,
             pair.now_ms) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 16u);
  pair.now_ms += pair.active.association->rto_ms;
  uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
  assert(h2_pal_sctp_association_service(pair.active.api,
                                         pair.active.association, pair.now_ms,
                                         &deadline) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 16u);
  assert(pair.passive.message_count == 1u);
  fallback_message.data = fallback;
  fallback_message.len = sizeof(fallback);
  fallback_message.stream_id = 7u;
  fallback_message.reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE;
  fallback_message.reliability_value = 0u;
  assert(h2_pal_sctp_association_send_message(
             pair.active.api, pair.active.association, &fallback_message,
             pair.now_ms) == H2_PAL_ERR_UNSUPPORTED);
  h2_sctp_test_pair_deinit(&pair);
  return 0;
}
