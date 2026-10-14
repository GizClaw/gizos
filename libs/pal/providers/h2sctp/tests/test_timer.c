#include "h2_sctp_internal.h"
#include "h2_sctp_test_peer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <string.h>

/* A blocked receiver must not suppress retransmission in the other
 * direction. Exercise real DATA/SACK packets with one deliberately lost DATA. */
static void blocked_delivery_keeps_retransmission_alive(void) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    uint8_t incoming[100], outgoing[100];
    memset(incoming, 0x33, sizeof(incoming));
    memset(outgoing, 0x77, sizeof(outgoing));
    h2_pal_sctp_message_t message = {
        .data = incoming, .len = sizeof(incoming), .stream_id = 1u,
        .ppid = 53u, .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
    };
    pair.passive.message_would_block_count = UINT_MAX;
    assert(h2_pal_sctp_association_send_message(pair.active.api,
               pair.active.association, &message, pair.now_ms) == H2_PAL_OK);
    assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) > 0u);
    assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) > 0u);
    assert(pair.passive.association->delivery_pending && pair.passive.message_count == 0u);
    message.data = outgoing;
    message.len = sizeof(outgoing);
    assert(h2_pal_sctp_association_send_message(pair.passive.api,
               pair.passive.association, &message, pair.now_ms) == H2_PAL_OK);
    pair.passive.drop_next = 1u;
    assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) == 0u);
    assert(pair.active.message_count == 0u && pair.passive.packet_head == NULL);
    pair.now_ms += pair.passive.association->rto_ms;
    uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
    const h2_pal_result_t result = h2_pal_sctp_association_service(
        pair.passive.api, pair.passive.association, pair.now_ms, &deadline);
    assert(pair.passive.packet_head != NULL);
    assert(result == H2_PAL_OK);
    assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) > 0u);
    assert(pair.active.message_count == 1u && pair.active.messages[0].len == sizeof(outgoing));
    assert(memcmp(pair.active.messages[0].data, outgoing, sizeof(outgoing)) == 0);
    assert(h2_sctp_test_transfer(&pair.active, &pair.passive, pair.now_ms) > 0u);
    assert(pair.passive.message_count == 0u);
    pair.passive.message_would_block_count = 0u;
    assert(h2_pal_sctp_association_service(pair.passive.api,
               pair.passive.association, ++pair.now_ms, &deadline) == H2_PAL_OK);
    assert(pair.passive.message_count == 1u && pair.passive.messages[0].len == sizeof(incoming));
    assert(memcmp(pair.passive.messages[0].data, incoming, sizeof(incoming)) == 0);
    assert(pair.passive.association->receive_used == 0u);
    h2_sctp_test_pair_deinit(&pair);
}

/* Reopening a zero receive window must notify the sender without waiting
 * for a new incoming packet or a probe timeout. */
static void resumed_delivery_reopens_receive_window(void) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 256u, 512u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    pair.passive.message_would_block_count = UINT_MAX;
    uint8_t data[512];
    memset(data, 0x5a, sizeof(data));
    size_t remaining = pair.passive.association->config.receive_buffer_size;
    size_t messages = 0u;
    while (remaining != 0u) {
        const size_t size = remaining < sizeof(data) ? remaining : sizeof(data);
        const h2_pal_sctp_message_t message = {
            .data = data, .len = size, .stream_id = 1u, .ppid = 53u,
            .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
        };
        assert(h2_pal_sctp_association_send_message(pair.active.api,
                   pair.active.association, &message, pair.now_ms) == H2_PAL_OK);
        (void)h2_sctp_test_pump(&pair, 32u);
        remaining -= size;
        ++messages;
    }
    assert(pair.passive.message_count == 0u);
    assert(pair.active.association->peer_receive_window == 0u);
    assert(pair.passive.packet_head == NULL);
    pair.passive.message_would_block_count = 0u;
    uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
    assert(h2_pal_sctp_association_service(pair.passive.api,
               pair.passive.association, ++pair.now_ms, &deadline) == H2_PAL_OK);
    assert(pair.passive.message_count == messages);
    assert(pair.passive.packet_head != NULL);
    assert(h2_sctp_test_transfer(&pair.passive, &pair.active, pair.now_ms) > 0u);
    assert(pair.active.association->peer_receive_window ==
           pair.passive.association->config.receive_buffer_size);
    for (size_t i = 0u; i < messages; ++i)
        assert(memcmp(pair.passive.messages[i].data, data,
                      pair.passive.messages[i].len) == 0);
    h2_sctp_test_pair_deinit(&pair);
}

#if H2_SCTP_TX_DIAGNOSTICS
static void bounded_diagnostic_sampling(void) {
    h2_pal_sctp_association_t a = {0};
    h2_sctp_diagnostic_sample(&a, 0u);
    assert(a.diagnostic_count == 0u);
    a.state = H2_PAL_SCTP_STATE_CONNECTED;
    a.cwnd = 4096u;
    a.peer_receive_window = 8192u;
    a.send_used = 123u;
    h2_sctp_diagnostic_sample(&a, 1000u);
    h2_sctp_diagnostic_sample(&a, 999u);
    h2_sctp_diagnostic_sample(&a, 1999u);
    assert(a.diagnostic_count == 1u);
    assert(a.diagnostics[0].cwnd == 4096u &&
           a.diagnostics[0].peer_window == 8192u &&
           a.diagnostics[0].queued == 123u);
    for (size_t i = 1u; i <= H2_SCTP_TX_DIAGNOSTIC_CAPACITY; ++i)
        h2_sctp_diagnostic_sample(&a, (i + 1u) * 1000u);
    assert(a.diagnostic_count == H2_SCTP_TX_DIAGNOSTIC_CAPACITY);
    assert(a.diagnostic_overflow == 1u);
    h2_sctp_diagnostic_sample(&a, a.diagnostic_last_ms);
    assert(a.diagnostic_overflow == 1u);
    assert(a.send_used == 123u && a.cwnd == 4096u);
}
#endif

int main(void) {
#if H2_SCTP_TX_DIAGNOSTICS
    bounded_diagnostic_sampling();
#endif
    blocked_delivery_keeps_retransmission_alive();
    resumed_delivery_reopens_receive_window();
    h2_sctp_test_endpoint_t endpoint;
    assert(h2_sctp_test_endpoint_init(
               &endpoint,
               H2_PAL_SCTP_ROLE_ACTIVE,
               5000u,
               5001u,
               256u,
               1024u,
               4096u,
               4096u) == H2_PAL_OK);
    endpoint.emit_would_block_count = 1u;
    assert(h2_pal_sctp_association_start(
               endpoint.api, endpoint.association, 10u) == H2_PAL_OK);
    uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
    assert(h2_pal_sctp_association_service(
               endpoint.api, endpoint.association, 10u, &deadline) ==
           H2_PAL_OK);
    assert(deadline == 1010u);
    assert(endpoint.packet_head != NULL);
    assert(h2_pal_sctp_association_service(
               endpoint.api, endpoint.association, 9u, &deadline) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(deadline == H2_PAL_SCTP_NO_DEADLINE);
    assert(h2_sctp_deadline_add(UINT64_MAX - 10u, 20u) == UINT64_MAX - 1u);
    h2_sctp_test_endpoint_deinit(&endpoint);

    assert(h2_sctp_test_endpoint_init(
               &endpoint,
               H2_PAL_SCTP_ROLE_ACTIVE,
               5000u,
               5001u,
               256u,
               1024u,
               4096u,
               4096u) == H2_PAL_OK);
    assert(h2_pal_sctp_association_start(
               endpoint.api, endpoint.association, 1u) == H2_PAL_OK);
    uint64_t now = 1u;
    for (unsigned attempt = 0u; attempt < 6u; ++attempt) {
        now += 60000u;
        (void)h2_pal_sctp_association_service(
            endpoint.api, endpoint.association, now, &deadline);
    }
    assert(endpoint.state == H2_PAL_SCTP_STATE_FAILED);
    assert(endpoint.state_reason == H2_PAL_ERR_TIMEOUT);
    h2_sctp_test_endpoint_deinit(&endpoint);
    return 0;
}
