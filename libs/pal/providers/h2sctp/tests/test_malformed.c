#include "h2_sctp_internal.h"
#include "h2_sctp_test_peer.h"
#include "h2_sctp_wire.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    const unsigned state_events = pair.passive.state_events;
    const size_t message_count = pair.passive.message_count;
    uint8_t packet[64] = {0};
    uint8_t chunk[4] = {0x80u, 0u, 0u, 4u};
    size_t packet_len = 0u;
    assert(h2_sctp_wire_build_packet(
               5000u,
               5001u,
               pair.passive.association->local_verification_tag,
               chunk,
               sizeof(chunk),
               packet,
               sizeof(packet),
               &packet_len) == H2_PAL_OK);
    assert(h2_pal_sctp_association_input_packet(
               pair.passive.api,
               pair.passive.association,
               packet,
               packet_len,
               pair.now_ms) == H2_PAL_OK);
    packet[8] ^= 1u;
    assert(h2_pal_sctp_association_input_packet(
               pair.passive.api,
               pair.passive.association,
               packet,
               packet_len,
               pair.now_ms) == H2_PAL_ERR_FORMAT);
    assert(h2_pal_sctp_association_input_packet(
               pair.passive.api,
               pair.passive.association,
               packet,
               11u,
               pair.now_ms) == H2_PAL_ERR_INVALID_ARG);
    chunk[2] = 0u;
    chunk[3] = 3u;
    assert(h2_sctp_wire_build_packet(
               5000u,
               5001u,
               pair.passive.association->local_verification_tag,
               chunk,
               sizeof(chunk),
               packet,
               sizeof(packet),
               &packet_len) == H2_PAL_OK);
    assert(h2_pal_sctp_association_input_packet(
               pair.passive.api,
               pair.passive.association,
               packet,
               packet_len,
               pair.now_ms) == H2_PAL_ERR_FORMAT);
    chunk[0] = 0x80u;
    chunk[2] = 0u;
    chunk[3] = 8u;
    assert(h2_sctp_wire_build_packet(
               5000u,
               5001u,
               pair.passive.association->local_verification_tag,
               chunk,
               sizeof(chunk),
               packet,
               sizeof(packet),
               &packet_len) == H2_PAL_OK);
    assert(h2_pal_sctp_association_input_packet(
               pair.passive.api,
               pair.passive.association,
               packet,
               packet_len,
               pair.now_ms) == H2_PAL_ERR_TRUNCATED);
    uint8_t bundled_init[24] = {
        1u, 0u, 0u, 20u,
        0x11u, 0x22u, 0x33u, 0x44u,
        0u, 0u, 16u, 0u,
        0u, 1u, 0u, 1u,
        1u, 2u, 3u, 4u,
        0x80u, 0u, 0u, 4u,
    };
    assert(h2_sctp_wire_build_packet(
               5000u,
               5001u,
               0u,
               bundled_init,
               sizeof(bundled_init),
               packet,
               sizeof(packet),
               &packet_len) == H2_PAL_OK);
    assert(h2_pal_sctp_association_input_packet(
               pair.passive.api,
               pair.passive.association,
               packet,
               packet_len,
               pair.now_ms) == H2_PAL_ERR_FORMAT);
    chunk[2] = 0u;
    chunk[3] = 4u;
    assert(h2_sctp_wire_build_packet(
               5000u,
               5001u,
               pair.passive.association->local_verification_tag + 1u,
               chunk,
               sizeof(chunk),
               packet,
               sizeof(packet),
               &packet_len) == H2_PAL_OK);
    assert(h2_pal_sctp_association_input_packet(
               pair.passive.api,
               pair.passive.association,
               packet,
               packet_len,
               pair.now_ms) == H2_PAL_OK);
    assert(pair.passive.state_events == state_events);
    assert(pair.passive.message_count == message_count);
    h2_sctp_test_pair_deinit(&pair);
    return 0;
}
