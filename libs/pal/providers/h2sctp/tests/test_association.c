#include "h2_sctp_internal.h"
#include "h2_sctp_test_peer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

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
               pair.passive.api,
               pair.passive.association,
               &message,
               pair.now_ms) == H2_PAL_OK);

    pair.now_ms++;
    assert(h2_pal_sctp_association_shutdown(
               pair.active.api, pair.active.association, pair.now_ms) ==
           H2_PAL_OK);
    (void)h2_sctp_test_pump(&pair, 32u);
    assert(pair.active.message_count == 1u);
    assert(pair.active.state == H2_PAL_SCTP_STATE_CLOSED);
    assert(pair.passive.state == H2_PAL_SCTP_STATE_CLOSED);
    h2_sctp_test_pair_deinit(&pair);
}

static void test_cookie_expiry(void) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
    assert(h2_pal_sctp_association_start(
               pair.passive.api, pair.passive.association, pair.now_ms) ==
           H2_PAL_OK);
    assert(h2_pal_sctp_association_start(
               pair.active.api, pair.active.association, pair.now_ms) ==
           H2_PAL_OK);
    assert(h2_sctp_test_transfer(
               &pair.active, &pair.passive, pair.now_ms) == 1u);
    assert(h2_sctp_test_transfer(
               &pair.passive, &pair.active, pair.now_ms) == 1u);
    pair.now_ms = 70000u;
    assert(h2_sctp_test_transfer(
               &pair.active, &pair.passive, pair.now_ms) == 1u);
    assert(pair.passive.state == H2_PAL_SCTP_STATE_CONNECTING);
    h2_sctp_test_pair_deinit(&pair);
}

static void test_abort_and_fresh_association(void) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 1200u, 32768u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    pair.now_ms++;
    pair.active.emit_would_block_count = 1u;
    assert(h2_pal_sctp_association_abort(
               pair.active.api,
               pair.active.association,
               H2_PAL_ERR_IO,
               pair.now_ms) == H2_PAL_OK);
    assert(pair.active.state == H2_PAL_SCTP_STATE_FAILED);
    assert(pair.active.state_reason == H2_PAL_ERR_IO);
    assert(pair.active.association->pending_emit != NULL);
    uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
    assert(h2_pal_sctp_association_service(
               pair.active.api,
               pair.active.association,
               pair.now_ms,
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

int main(void) {
    test_connect_and_shutdown();
    test_cookie_expiry();
    test_abort_and_fresh_association();
    return 0;
}
