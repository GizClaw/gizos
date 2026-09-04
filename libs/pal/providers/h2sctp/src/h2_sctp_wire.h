#ifndef H2_SCTP_WIRE_H
#define H2_SCTP_WIRE_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#define H2_SCTP_COMMON_HEADER_SIZE 12u
#define H2_SCTP_CHUNK_HEADER_SIZE 4u

typedef struct h2_sctp_packet_view {
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t verification_tag;
    const uint8_t *chunks;
    size_t chunks_len;
} h2_sctp_packet_view_t;

typedef struct h2_sctp_chunk_view {
    uint8_t type;
    uint8_t flags;
    const uint8_t *data;
    size_t len;
    size_t padded_len;
} h2_sctp_chunk_view_t;

uint16_t h2_sctp_wire_read_u16(const uint8_t *data);
uint32_t h2_sctp_wire_read_u32(const uint8_t *data);
void h2_sctp_wire_write_u16(uint8_t *data, uint16_t value);
void h2_sctp_wire_write_u32(uint8_t *data, uint32_t value);

h2_pal_result_t h2_sctp_wire_parse_packet(
    const uint8_t *packet,
    size_t packet_len,
    h2_sctp_packet_view_t *out_packet);

h2_pal_result_t h2_sctp_wire_parse_chunk(
    const uint8_t *chunks,
    size_t chunks_len,
    size_t offset,
    h2_sctp_chunk_view_t *out_chunk);

/* Writes the 12-byte common header with a zero checksum field. */
void h2_sctp_wire_write_common_header(
    uint8_t *out_packet,
    uint16_t source_port,
    uint16_t destination_port,
    uint32_t verification_tag);
/* Computes and stores the CRC32C of a packet whose chunks are in place. */
void h2_sctp_wire_finish_packet(uint8_t *packet, size_t packet_len);

h2_pal_result_t h2_sctp_wire_build_packet(
    uint16_t source_port,
    uint16_t destination_port,
    uint32_t verification_tag,
    const uint8_t *chunks,
    size_t chunks_len,
    uint8_t *out_packet,
    size_t packet_capacity,
    size_t *out_packet_len);

#endif
