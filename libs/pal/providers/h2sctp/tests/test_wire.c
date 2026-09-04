#include "h2_sctp_crc32c.h"
#include "h2_sctp_reference_vectors.h"
#include "h2_sctp_wire.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

/* RFC 3720 Appendix B.4 CRC32C known-answer vectors. */
static void crc32c_known_answers(void) {
    uint8_t zeros[32];
    uint8_t ones[32];
    uint8_t incrementing[32];
    uint8_t decrementing[32];
    for (size_t index = 0u; index < 32u; ++index) {
        zeros[index] = 0u;
        ones[index] = 0xffu;
        incrementing[index] = (uint8_t)index;
        decrementing[index] = (uint8_t)(31u - index);
    }
    static const uint8_t iscsi_read_command[48] = {
        0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18,
        0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    assert(h2_sctp_crc32c(zeros, sizeof(zeros)) == 0x8a9136aau);
    assert(h2_sctp_crc32c(ones, sizeof(ones)) == 0x62a8ab43u);
    assert(h2_sctp_crc32c(incrementing, sizeof(incrementing)) == 0x46dd794eu);
    assert(h2_sctp_crc32c(decrementing, sizeof(decrementing)) == 0x113fdb5cu);
    assert(h2_sctp_crc32c(iscsi_read_command, sizeof(iscsi_read_command)) ==
           0xd9963a56u);
    assert(h2_sctp_crc32c(zeros, 0u) == 0u);
    assert(h2_sctp_crc32c(NULL, 0u) == 0u);
    assert(h2_sctp_crc32c(NULL, 1u) == 0u);
}

static uint32_t crc32c_bit_serial(const uint8_t *data, size_t len) {
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0u; index < len; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0x82f63b78u & mask);
        }
    }
    return ~crc;
}

/* The sliced implementation must agree with the reference for every start
 * alignment and every tail length, including a packet-sized buffer. */
static void crc32c_matches_bit_serial_reference(void) {
    static uint8_t buffer[1500 + 8];
    uint32_t seed = 0x12345678u;
    for (size_t index = 0u; index < sizeof(buffer); ++index) {
        seed = seed * 1103515245u + 12345u;
        buffer[index] = (uint8_t)(seed >> 16u);
    }
    for (size_t start = 0u; start < 8u; ++start) {
        for (size_t len = 0u; len < 40u; ++len) {
            assert(h2_sctp_crc32c(buffer + start, len) ==
                   crc32c_bit_serial(buffer + start, len));
        }
        assert(h2_sctp_crc32c(buffer + start, 1500u) ==
               crc32c_bit_serial(buffer + start, 1500u));
    }
}

int main(void) {
    static const uint8_t check[] = "123456789";
    assert(h2_sctp_crc32c(check, sizeof(check) - 1u) == 0xe3069283u);
    crc32c_known_answers();
    crc32c_matches_bit_serial_reference();

    h2_sctp_packet_view_t packet;
    assert(h2_sctp_wire_parse_packet(
               h2_sctp_reference_init_packet,
               h2_sctp_reference_init_packet_len,
               &packet) == H2_PAL_OK);
    assert(packet.source_port == 5000u);
    assert(packet.destination_port == 5001u);
    assert(packet.verification_tag == 0u);

    h2_sctp_chunk_view_t chunk;
    assert(h2_sctp_wire_parse_chunk(
               packet.chunks, packet.chunks_len, 0u, &chunk) == H2_PAL_OK);
    assert(chunk.type == 1u);
    assert(chunk.len == 16u);
    assert(h2_sctp_wire_read_u32(chunk.data) == 0x11223344u);
    assert(h2_sctp_wire_read_u32(chunk.data + 12u) == 0x01020304u);

    uint8_t unaligned_storage[64] = {0};
    uint8_t *unaligned = unaligned_storage + 1u;
    size_t packet_len = 0u;
    assert(h2_sctp_wire_build_packet(
               5000u,
               5001u,
               0u,
               packet.chunks,
               packet.chunks_len,
               unaligned,
               sizeof(unaligned_storage) - 1u,
               &packet_len) == H2_PAL_OK);
    assert(packet_len == h2_sctp_reference_init_packet_len);
    assert(memcmp(
               unaligned,
               h2_sctp_reference_init_packet,
               packet_len) == 0);

    uint8_t padded[] = {64u, 3u, 0u, 5u, 0xa5u, 0u, 0u, 0u};
    assert(h2_sctp_wire_parse_chunk(
               padded, sizeof(padded), 0u, &chunk) == H2_PAL_OK);
    padded[7] = 1u;
    assert(h2_sctp_wire_parse_chunk(
               padded, sizeof(padded), 0u, &chunk) == H2_PAL_ERR_FORMAT);

    assert(h2_sctp_wire_parse_chunk(
               h2_sctp_reference_i_data_chunk,
               h2_sctp_reference_i_data_chunk_len,
               0u,
               &chunk) == H2_PAL_OK);
    assert(chunk.type == 64u && chunk.flags == 3u && chunk.len == 20u);
    assert(h2_sctp_wire_read_u32(chunk.data) == 0x10203040u);
    assert(h2_sctp_wire_read_u32(chunk.data + 12u) == 53u);
    assert(h2_sctp_wire_parse_chunk(
               h2_sctp_reference_i_forward_tsn_chunk,
               h2_sctp_reference_i_forward_tsn_chunk_len,
               0u,
               &chunk) == H2_PAL_OK);
    assert(chunk.type == 194u && chunk.len == 12u);
    assert(h2_sctp_wire_parse_chunk(
               h2_sctp_reference_reset_chunk,
               h2_sctp_reference_reset_chunk_len,
               0u,
               &chunk) == H2_PAL_OK);
    assert(chunk.type == 130u && chunk.len == 18u);
    assert(h2_sctp_wire_read_u16(chunk.data) == 13u);
    assert(h2_sctp_wire_parse_chunk(
               h2_sctp_reference_shutdown_chunk,
               h2_sctp_reference_shutdown_chunk_len,
               0u,
               &chunk) == H2_PAL_OK);
    assert(chunk.type == 7u && chunk.len == 4u);
    return 0;
}
