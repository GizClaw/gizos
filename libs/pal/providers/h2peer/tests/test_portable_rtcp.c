#include "rtcp.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_pli_bytes(void) {
    uint8_t packet[12];
    assert(rtcp_get_pli(packet, sizeof(packet), UINT32_C(0x01020304)) == 12);
    const uint8_t expected[] = {
        0x81u, 0xceu, 0x00u, 0x02u,
        0x00u, 0x00u, 0x00u, 0x00u,
        0x01u, 0x02u, 0x03u, 0x04u,
    };
    assert(memcmp(packet, expected, sizeof(expected)) == 0);

    RtcpHeader header;
    assert(rtcp_parse_header(packet, sizeof(packet), &header) == 0);
    assert(header.version == 2u && header.rc == 1u &&
           header.type == RTCP_PSFB && header.length == 2u);
}

static void test_fir_bytes(void) {
    uint8_t packet[20];
    int sequence = 254;
    assert(rtcp_get_fir(packet, sizeof(packet), &sequence) == 20);
    assert(packet[0] == 0x84u && packet[1] == 0xceu &&
           packet[2] == 0x00u && packet[3] == 0x04u &&
           packet[16] == 0xffu);
    assert(rtcp_get_fir(packet, sizeof(packet), &sequence) == 20);
    assert(sequence == 0 && packet[16] == 0u);
}

static void test_malformed_header(void) {
    uint8_t packet[] = {0x80u, 0xc9u, 0xffu, 0xffu, 0u, 0u, 0u, 0u};
    RtcpHeader header;
    assert(rtcp_parse_header(packet, sizeof(packet), &header) != 0);
    assert(rtcp_probe(packet, sizeof(packet)) == 0);
}

int main(void) {
    test_pli_bytes();
    test_fir_bytes();
    test_malformed_header();
    return 0;
}
