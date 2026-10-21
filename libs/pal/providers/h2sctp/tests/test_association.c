#include "h2_sctp_internal.h"
#include "h2_sctp_test_peer.h"
#include "h2_sctp_wire.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static void test_connect_and_shutdown(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  pair.active.duplicate_next = 1u;
  assert(h2_sctp_test_connect(&pair));
  assert(pair.active.state_events == 2u);
  assert(pair.passive.state_events == 2u);

  const uint8_t in_flight[] = {1u, 2u, 3u, 4u};
  const h2_pal_sctp_message_t message = {
      .data = in_flight,
      .len = sizeof(in_flight),
      .stream_id = 1u,
      .ppid = 53u,
      .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
  };
  assert(h2_pal_sctp_association_send_message(
             pair.passive.api, pair.passive.association, &message,
             pair.now_ms) == H2_PAL_OK);

  pair.now_ms++;
  assert(h2_pal_sctp_association_shutdown(pair.active.api,
                                          pair.active.association,
                                          pair.now_ms) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 32u);
  assert(pair.active.message_count == 1u);
  assert(pair.active.state == H2_PAL_SCTP_STATE_CLOSED);
  assert(pair.passive.state == H2_PAL_SCTP_STATE_CLOSED);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_cookie_expiry(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.passive.api,
                                       pair.passive.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.active.api, pair.active.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) == 1u);
  pair.now_ms = 70000u;
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  assert(pair.passive.state == H2_PAL_SCTP_STATE_CONNECTING);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_lost_cookie_ack(bool block_repeated_ack) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.passive.api,
                                       pair.passive.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.active.api, pair.active.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) == 1u);
  const h2_sctp_test_packet_t* echo = pair.active.packet_head;
  uint8_t echoed_packet[128];
  assert(echo != NULL && echo->len <= sizeof(echoed_packet));
  const size_t echo_len = echo->len;
  memcpy(echoed_packet, echo->data, echo_len);
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  assert(pair.passive.state == H2_PAL_SCTP_STATE_CONNECTED);
  pair.passive.drop_next = 1u;
  assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) == 0u);
  assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTING);
  const uint64_t heartbeat = pair.passive.association->heartbeat_deadline_ms;

  /* The active endpoint's real T1-cookie timer retransmits the same ECHO
   * after the passive endpoint already reported CONNECTED. */
  pair.now_ms += 4000u;
  uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
  assert(h2_pal_sctp_association_service(pair.active.api,
                                         pair.active.association, pair.now_ms,
                                         &deadline) == H2_PAL_OK);
  pair.passive.emit_would_block_count = block_repeated_ack ? 1u : 0u;
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  (void)h2_sctp_test_pump(&pair, 8u);
  assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
  assert(pair.active.state_events == 2u && pair.passive.state_events == 2u);
  assert(pair.passive.association->heartbeat_deadline_ms == heartbeat);
  const uint8_t payload[] = {1u, 2u, 3u, 4u};
  const h2_pal_sctp_message_t message = {
      .data = payload,
      .len = sizeof(payload),
      .stream_id = 1u,
      .ppid = 53u,
      .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
  };
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 8u);
  assert(pair.passive.message_count == 1u);

  /* An established association still recognizes its exact cookie after the
   * initial setup deadline; replying must not emit another state event. */
  pair.now_ms = 70000u;
  assert(h2_pal_sctp_association_input_packet(
             pair.passive.api, pair.passive.association, echoed_packet,
             echo_len, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.packet_head != NULL);
  (void)h2_sctp_test_pump(&pair, 8u);
  assert(pair.active.state_events == 2u && pair.passive.state_events == 2u);
  assert(h2_pal_sctp_association_send_message(pair.active.api,
                                              pair.active.association, &message,
                                              pair.now_ms) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 8u);
  assert(pair.passive.message_count == 2u);
  assert(pair.passive.messages[1].len == sizeof(payload));
  assert(memcmp(pair.passive.messages[1].data, payload, sizeof(payload)) == 0);

  /* A different cookie with a valid packet CRC and Verification Tag is not
   * authenticated by an already-established association. */
  uint8_t chunk[4u + sizeof(pair.passive.association->cookie)];
  chunk[0] = H2_SCTP_CHUNK_COOKIE_ECHO;
  chunk[1] = 0u;
  h2_sctp_wire_write_u16(chunk + 2u, (uint16_t)sizeof(chunk));
  memcpy(chunk + 4u, pair.passive.association->cookie,
         sizeof(pair.passive.association->cookie));
  chunk[4] ^= 1u;
  uint8_t wrong_cookie[128];
  size_t wrong_len = 0u;
  assert(h2_sctp_wire_build_packet(
             5000u, 5001u, pair.passive.association->local_verification_tag,
             chunk, sizeof(chunk), wrong_cookie, sizeof(wrong_cookie),
             &wrong_len) == H2_PAL_OK);
  assert(h2_pal_sctp_association_input_packet(
             pair.passive.api, pair.passive.association, wrong_cookie,
             wrong_len, pair.now_ms) == H2_PAL_OK);
  assert(pair.passive.packet_head == NULL);
  assert(pair.passive.state_events == 2u);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_abort_and_fresh_association(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  pair.now_ms++;
  pair.active.emit_would_block_count = 1u;
  assert(h2_pal_sctp_association_abort(pair.active.api, pair.active.association,
                                       H2_PAL_ERR_IO,
                                       pair.now_ms) == H2_PAL_OK);
  assert(pair.active.state == H2_PAL_SCTP_STATE_FAILED);
  assert(pair.active.state_reason == H2_PAL_ERR_IO);
  assert(pair.active.association->pending_emit != NULL);
  uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
  assert(h2_pal_sctp_association_service(pair.active.api,
                                         pair.active.association, pair.now_ms,
                                         &deadline) == H2_PAL_OK);
  assert(pair.active.association->pending_emit == NULL);
  assert(pair.active.packet_head != NULL);
  (void)h2_sctp_test_pump(&pair, 8u);
  assert(pair.passive.state == H2_PAL_SCTP_STATE_FAILED);
  h2_sctp_test_pair_deinit(&pair);

  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  h2_sctp_test_pair_deinit(&pair);
}

static void test_simultaneous_active_open(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  h2_sctp_test_endpoint_deinit(&pair.passive);
  assert(h2_sctp_test_endpoint_init(&pair.passive, H2_PAL_SCTP_ROLE_ACTIVE,
                                    5001u, 5000u, 1200u, 32768u, 132272u,
                                    132272u) == H2_PAL_OK);
  assert(h2_sctp_test_connect(&pair));
  /* A peer can finish via COOKIE-ECHO before its own COOKIE-ACK arrives.
   * Neither side may retain a setup timer on an established association. */
  assert(pair.active.association->control_kind == H2_SCTP_CONTROL_NONE);
  assert(pair.passive.association->control_kind == H2_SCTP_CONTROL_NONE);
  pair.now_ms += 100000u;
  (void)h2_sctp_test_pump(&pair, 8u);
  assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
  assert(pair.passive.state == H2_PAL_SCTP_STATE_CONNECTED);
  assert(pair.active.state_events == 2u && pair.passive.state_events == 2u);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_reordered_init_acks(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.passive.api,
                                       pair.passive.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.active.api, pair.active.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  uint8_t first_ack[128];
  const h2_sctp_test_packet_t* ack = pair.passive.packet_head;
  assert(ack != NULL && ack->len <= sizeof(first_ack));
  const size_t ack_len = ack->len;
  memcpy(first_ack, ack->data, ack_len);
  pair.passive.drop_next = 1u;
  assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) == 0u);

  pair.now_ms += 4000u;
  uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
  assert(h2_pal_sctp_association_service(pair.active.api,
                                         pair.active.association, pair.now_ms,
                                         &deadline) == H2_PAL_OK);
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  /* Deliver the original ACK late, and lose the duplicate ACK. Pion can
   * legitimately keep the first cookie and ignore later INIT-ACKs. */
  pair.passive.drop_next = 1u;
  assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) == 0u);
  assert(h2_pal_sctp_association_input_packet(
             pair.active.api, pair.active.association, first_ack, ack_len,
             pair.now_ms) == H2_PAL_OK);
  (void)h2_sctp_test_pump(&pair, 8u);
  assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
  assert(pair.passive.state == H2_PAL_SCTP_STATE_CONNECTED);
  h2_sctp_test_pair_deinit(&pair);
}

static void test_init_ack_after_cookie_echo(void) {
  h2_sctp_test_pair_t pair;
  assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.passive.api,
                                       pair.passive.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_pal_sctp_association_start(pair.active.api, pair.active.association,
                                       pair.now_ms) == H2_PAL_OK);
  assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) == 1u);
  uint8_t init_ack[128];
  const h2_sctp_test_packet_t* ack = pair.passive.packet_head;
  assert(ack != NULL && ack->len <= sizeof(init_ack));
  const size_t ack_len = ack->len;
  memcpy(init_ack, ack->data, ack_len);
  assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) == 1u);
  assert(pair.active.packet_head != NULL &&
         pair.active.packet_head->next == NULL);
  assert(h2_pal_sctp_association_input_packet(
             pair.active.api, pair.active.association, init_ack, ack_len,
             pair.now_ms) == H2_PAL_OK);
  assert(pair.active.packet_head->next == NULL);
  assert(h2_sctp_test_pump(&pair, 8u) != 0u);
  assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
  h2_sctp_test_pair_deinit(&pair);
}

int main(void) {
  test_connect_and_shutdown();
  test_cookie_expiry();
  test_lost_cookie_ack(false);
  test_lost_cookie_ack(true);
  test_simultaneous_active_open();
  test_reordered_init_acks();
  test_init_ack_after_cookie_echo();
  test_abort_and_fresh_association();
  return 0;
}
