#include "h2_sctp_reference_vectors.h"
#include "h2_sctp_test_peer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
    assert(h2_sctp_reference_init_packet_len == 32u);
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    static const uint8_t ordered[] = "ordered-rfc8260";
    static const uint8_t unordered[] = "unordered-rfc8260";
    const h2_pal_sctp_message_t messages[] = {
        {
            .data = ordered,
            .len = sizeof(ordered),
            .stream_id = 4u,
            .ppid = 53u,
            .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
        },
        {
            .data = unordered,
            .len = sizeof(unordered),
            .stream_id = 5u,
            .ppid = 51u,
            .unordered = true,
            .reliability = H2_PAL_SCTP_RELIABILITY_MAX_LIFETIME_MS,
            .reliability_value = 5000u,
        },
    };
    for (size_t index = 0u; index < 2u; ++index) {
        assert(h2_pal_sctp_association_send_message(
                   pair.active.api,
                   pair.active.association,
                   &messages[index],
                   pair.now_ms) == H2_PAL_OK);
    }
    pair.active.reorder_next = true;
    (void)h2_sctp_test_pump(&pair, 32u);
    assert(pair.passive.message_count == 2u);
    assert(pair.passive.messages[0].ppid == 51u ||
           pair.passive.messages[0].ppid == 53u);
    assert(h2_pal_sctp_association_reset_stream(
               pair.active.api,
               pair.active.association,
               4u,
               pair.now_ms) == H2_PAL_OK);
    (void)h2_sctp_test_pump(&pair, 32u);
    assert(pair.active.outgoing_reset_events == 1u);
    assert(pair.passive.incoming_reset_events == 1u);
    assert(h2_pal_sctp_association_shutdown(
               pair.active.api,
               pair.active.association,
               pair.now_ms) == H2_PAL_OK);
    (void)h2_sctp_test_pump(&pair, 32u);
    assert(pair.active.state == H2_PAL_SCTP_STATE_CLOSED);
    assert(pair.passive.state == H2_PAL_SCTP_STATE_CLOSED);
    h2_sctp_test_pair_deinit(&pair);
    return 0;
}
