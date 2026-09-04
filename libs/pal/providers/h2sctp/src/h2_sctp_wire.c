#include "h2_sctp_wire.h"

#include "h2_sctp_crc32c.h"

#include <string.h>

uint16_t h2_sctp_wire_read_u16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8u) | data[1]);
}

uint32_t h2_sctp_wire_read_u32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
           ((uint32_t)data[2] << 8u) | data[3];
}

static uint32_t h2_sctp_wire_read_crc(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

void h2_sctp_wire_write_u16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8u);
    data[1] = (uint8_t)value;
}

void h2_sctp_wire_write_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24u);
    data[1] = (uint8_t)(value >> 16u);
    data[2] = (uint8_t)(value >> 8u);
    data[3] = (uint8_t)value;
}

static void h2_sctp_wire_write_crc(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8u);
    data[2] = (uint8_t)(value >> 16u);
    data[3] = (uint8_t)(value >> 24u);
}

h2_pal_result_t h2_sctp_wire_parse_packet(
    const uint8_t *packet,
    size_t packet_len,
    h2_sctp_packet_view_t *out_packet) {
    if (out_packet == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_packet = (h2_sctp_packet_view_t){0};
    if (packet == NULL || packet_len < H2_SCTP_COMMON_HEADER_SIZE) {
        return H2_PAL_ERR_TRUNCATED;
    }
    const uint32_t checksum = h2_sctp_wire_read_crc(packet + 8u);
    if (checksum != h2_sctp_crc32c_packet(packet, packet_len)) {
        return H2_PAL_ERR_FORMAT;
    }
    out_packet->source_port = h2_sctp_wire_read_u16(packet);
    out_packet->destination_port = h2_sctp_wire_read_u16(packet + 2u);
    out_packet->verification_tag = h2_sctp_wire_read_u32(packet + 4u);
    out_packet->chunks = packet + H2_SCTP_COMMON_HEADER_SIZE;
    out_packet->chunks_len = packet_len - H2_SCTP_COMMON_HEADER_SIZE;
    return H2_PAL_OK;
}

h2_pal_result_t h2_sctp_wire_parse_chunk(
    const uint8_t *chunks,
    size_t chunks_len,
    size_t offset,
    h2_sctp_chunk_view_t *out_chunk) {
    if (out_chunk == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_chunk = (h2_sctp_chunk_view_t){0};
    if (chunks == NULL || offset > chunks_len ||
        chunks_len - offset < H2_SCTP_CHUNK_HEADER_SIZE) {
        return H2_PAL_ERR_TRUNCATED;
    }
    const uint8_t *chunk = chunks + offset;
    const size_t chunk_len = h2_sctp_wire_read_u16(chunk + 2u);
    if (chunk_len < H2_SCTP_CHUNK_HEADER_SIZE) {
        return H2_PAL_ERR_FORMAT;
    }
    if (chunk_len > chunks_len - offset) {
        return H2_PAL_ERR_TRUNCATED;
    }
    if (chunk_len > SIZE_MAX - 3u) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t padded_len = (chunk_len + 3u) & ~(size_t)3u;
    if (padded_len > chunks_len - offset) {
        return H2_PAL_ERR_TRUNCATED;
    }
    for (size_t index = chunk_len; index < padded_len; ++index) {
        if (chunk[index] != 0u) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    out_chunk->type = chunk[0];
    out_chunk->flags = chunk[1];
    out_chunk->data = chunk + H2_SCTP_CHUNK_HEADER_SIZE;
    out_chunk->len = chunk_len - H2_SCTP_CHUNK_HEADER_SIZE;
    out_chunk->padded_len = padded_len;
    return H2_PAL_OK;
}

void h2_sctp_wire_write_common_header(
    uint8_t *out_packet,
    uint16_t source_port,
    uint16_t destination_port,
    uint32_t verification_tag) {
    h2_sctp_wire_write_u16(out_packet, source_port);
    h2_sctp_wire_write_u16(out_packet + 2u, destination_port);
    h2_sctp_wire_write_u32(out_packet + 4u, verification_tag);
    memset(out_packet + 8u, 0, 4u);
}

void h2_sctp_wire_finish_packet(uint8_t *packet, size_t packet_len) {
    h2_sctp_wire_write_crc(
        packet + 8u, h2_sctp_crc32c_packet(packet, packet_len));
}

h2_pal_result_t h2_sctp_wire_build_packet(
    uint16_t source_port,
    uint16_t destination_port,
    uint32_t verification_tag,
    const uint8_t *chunks,
    size_t chunks_len,
    uint8_t *out_packet,
    size_t packet_capacity,
    size_t *out_packet_len) {
    if (out_packet_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_packet_len = 0u;
    if (source_port == 0u || destination_port == 0u || chunks == NULL ||
        chunks_len < H2_SCTP_CHUNK_HEADER_SIZE || out_packet == NULL ||
        chunks_len > SIZE_MAX - H2_SCTP_COMMON_HEADER_SIZE ||
        packet_capacity < H2_SCTP_COMMON_HEADER_SIZE + chunks_len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_sctp_wire_write_common_header(
        out_packet, source_port, destination_port, verification_tag);
    memcpy(out_packet + H2_SCTP_COMMON_HEADER_SIZE, chunks, chunks_len);
    const size_t packet_len = H2_SCTP_COMMON_HEADER_SIZE + chunks_len;
    h2_sctp_wire_finish_packet(out_packet, packet_len);
    *out_packet_len = packet_len;
    return H2_PAL_OK;
}
