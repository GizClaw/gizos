#include "data_channel/h2_peer_dcep.h"
#include "media/h2_peer_rtp.h"
#include "sdp/h2_peer_sdp.h"
#include "stun/h2_peer_stun.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t test_next(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void test_mutated_corpus(void) {
    uint8_t corpus[1601];
    uint32_t state = UINT32_C(0x48325052);
    for (size_t i = 0u; i < sizeof(corpus); ++i) {
        corpus[i] = (uint8_t)(test_next(&state) >> 24u);
    }

    for (size_t len = 0u; len <= sizeof(corpus); ++len) {
        h2_peer_rtp_packet_t rtp;
        h2_peer_stun_message_t stun;
        h2_peer_dcep_open_t dcep;
        h2_peer_sdp_description_t sdp;
        h2_pal_webrtc_str_t text = {
            .data = (const char *)corpus,
            .len = len,
        };
        (void)h2_peer_rtp_parse(corpus, len, &rtp);
        (void)h2_peer_rtcp_validate(corpus, len);
        (void)h2_peer_stun_parse(corpus, len, &stun);
        (void)h2_peer_dcep_parse_open(corpus, len, &dcep);
        (void)h2_peer_sdp_parse(text, &sdp);
    }
}

static void test_truncated_valid_prefixes(void) {
    uint8_t rtp[16] = {0x80u, 0x6fu, 0u, 1u, 0u, 0u, 0u, 2u, 0u, 0u, 0u, 3u, 0xf8u};
    uint8_t stun[20] = {0x00u, 0x01u, 0u, 0u, 0x21u, 0x12u, 0xa4u, 0x42u};
    uint8_t dcep[16] = {0x03u, 0x00u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 4u, 0u, 0u, 't', 'e', 's', 't'};
    for (size_t len = 0u; len < sizeof(rtp); ++len) {
        h2_peer_rtp_packet_t parsed;
        (void)h2_peer_rtp_parse(rtp, len, &parsed);
    }
    for (size_t len = 0u; len < sizeof(stun); ++len) {
        h2_peer_stun_message_t parsed;
        (void)h2_peer_stun_parse(stun, len, &parsed);
    }
    for (size_t len = 0u; len < sizeof(dcep); ++len) {
        h2_peer_dcep_open_t parsed;
        (void)h2_peer_dcep_parse_open(dcep, len, &parsed);
    }
}

int main(void) {
    test_mutated_corpus();
    test_truncated_valid_prefixes();
    return 0;
}
