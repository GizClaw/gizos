#include "h2_sctp_crc32c.h"
#include "h2_sctp_reference_vectors.h"
#include "h2_sctp_wire.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
    static const uint8_t check[] = "123456789";
    assert(h2_sctp_crc32c(check, sizeof(check) - 1u) == 0xe3069283u);

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
