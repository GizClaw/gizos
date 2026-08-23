#include "data_channel/h2_peer_dcep.h"
#include "media/h2_peer_rtp.h"
#include "sdp/h2_peer_sdp.h"
#include "stun/h2_peer_stun.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_rtp(void) {
    const uint8_t opus[] = {0xf8u, 0x01u, 0x02u};
    uint8_t packet[32];
    size_t packet_len = 99u;
    assert(h2_peer_rtp_write_opus(
               0x1234u,
               UINT32_C(0x01020304),
               UINT32_C(0xaabbccdd),
               opus,
               sizeof(opus),
               packet,
               sizeof(packet),
               &packet_len) == H2_PAL_OK);
    assert(packet[0] == 0x80u);
    assert(packet[1] == 0x6fu);
    assert(packet[2] == 0x12u && packet[3] == 0x34u);

    h2_peer_rtp_packet_t parsed;
    assert(h2_peer_rtp_parse(packet, packet_len, &parsed) == H2_PAL_OK);
    assert(parsed.payload_type == 111u);
    assert(parsed.sequence == 0x1234u);
    assert(parsed.timestamp == UINT32_C(0x01020304));
    assert(parsed.ssrc == UINT32_C(0xaabbccdd));
    assert(parsed.payload_len == sizeof(opus));
    assert(memcmp(parsed.payload, opus, sizeof(opus)) == 0);

    assert(h2_peer_rtp_parse(packet + 1u, packet_len - 1u, &parsed) != H2_PAL_OK);
    uint8_t oversized[1501] = {0x80u, 0x6fu};
    assert(h2_peer_rtp_parse(oversized, sizeof(oversized), &parsed) == H2_PAL_ERR_FORMAT);
}

static void test_rtcp(void) {
    const uint8_t receiver_report[] = {0x80u, 201u, 0x00u, 0x01u, 0u, 0u, 0u, 1u};
    assert(h2_peer_rtcp_validate(receiver_report, sizeof(receiver_report)) == H2_PAL_OK);
    assert(h2_peer_rtcp_validate(receiver_report, sizeof(receiver_report) - 1u) == H2_PAL_ERR_TRUNCATED);
}

static void test_stun(void) {
    const uint8_t transaction_id[12] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u};
    uint8_t packet[H2_PEER_STUN_HEADER_SIZE];
    size_t packet_len = 0u;
    assert(h2_peer_stun_write_binding_request(
               transaction_id, packet, sizeof(packet), &packet_len) == H2_PAL_OK);
    h2_peer_stun_message_t parsed;
    assert(h2_peer_stun_parse(packet, packet_len, &parsed) == H2_PAL_OK);
    assert(parsed.type == 1u && parsed.attribute_count == 0u);
    assert(memcmp(parsed.transaction_id, transaction_id, sizeof(transaction_id)) == 0);

    uint8_t oversized_attribute[H2_PEER_STUN_HEADER_SIZE + 516u] = {0};
    memcpy(oversized_attribute, packet, sizeof(packet));
    oversized_attribute[2] = 0x02u;
    oversized_attribute[3] = 0x04u;
    oversized_attribute[22] = 0x02u;
    oversized_attribute[23] = 0x01u;
    assert(h2_peer_stun_parse(
               oversized_attribute, sizeof(oversized_attribute), &parsed) == H2_PAL_ERR_NO_SPACE);
}

static void test_sdp(void) {
    const uint8_t random[16] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
    char sdp[1024];
    size_t sdp_len = 0u;
    const char fingerprint[] = "00:11:22:33";
    h2_pal_webrtc_str_t fingerprint_value = {
        .data = fingerprint,
        .len = sizeof(fingerprint) - 1u,
    };
    assert(h2_peer_sdp_write_offer(
               random,
               7u,
               fingerprint_value,
               sdp,
               sizeof(sdp),
               &sdp_len) == H2_PAL_OK);
    h2_peer_sdp_description_t parsed;
    h2_pal_webrtc_str_t value = {.data = sdp, .len = sdp_len};
    assert(h2_peer_sdp_parse(value, &parsed) == H2_PAL_OK);
    assert(parsed.has_opus && parsed.has_data_channel);

    char long_line[H2_PEER_SDP_LINE_MAX + 2u];
    memset(long_line, 'x', sizeof(long_line));
    value.data = long_line;
    value.len = sizeof(long_line);
    assert(h2_peer_sdp_parse(value, &parsed) == H2_PAL_ERR_FORMAT);
}

static void test_dcep(void) {
    const uint8_t label[] = "giznet/v1/packet";
    uint8_t packet[64];
    size_t packet_len = 0u;
    assert(h2_peer_dcep_write_open(
               label,
               sizeof(label) - 1u,
               0,
               0,
               packet,
               sizeof(packet),
               &packet_len) == H2_PAL_OK);
    h2_peer_dcep_open_t parsed;
    assert(h2_peer_dcep_parse_open(packet, packet_len, &parsed) == H2_PAL_OK);
    assert(!parsed.ordered && !parsed.reliable);
    assert(parsed.label_len == sizeof(label) - 1u);
    assert(h2_peer_dcep_parse_open(packet, packet_len - 1u, &parsed) == H2_PAL_ERR_TRUNCATED);

    uint8_t oversized[H2_PEER_DCEP_LABEL_MAX + 1u];
    memset(oversized, 'a', sizeof(oversized));
    assert(h2_peer_dcep_write_open(
               oversized,
               sizeof(oversized),
               1,
               1,
               packet,
               sizeof(packet),
               &packet_len) == H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
    test_rtp();
    test_rtcp();
    test_stun();
    test_sdp();
    test_dcep();
    return 0;
}
