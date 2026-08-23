#include "h2_peer_rtp.h"

#include <string.h>

static uint16_t h2_peer_read_be16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8u) | (uint16_t)data[1]);
}

static uint32_t h2_peer_read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
           ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static void h2_peer_write_be16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8u);
    data[1] = (uint8_t)value;
}

static void h2_peer_write_be32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24u);
    data[1] = (uint8_t)(value >> 16u);
    data[2] = (uint8_t)(value >> 8u);
    data[3] = (uint8_t)value;
}

h2_pal_result_t h2_peer_rtp_write_opus(
    uint16_t sequence,
    uint32_t timestamp,
    uint32_t ssrc,
    const uint8_t *opus,
    size_t opus_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (opus == NULL || opus_len == 0u || out == NULL || out_len == NULL ||
        opus_len > SIZE_MAX - H2_PEER_RTP_HEADER_SIZE ||
        out_cap < H2_PEER_RTP_HEADER_SIZE + opus_len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out[0] = 0x80u;
    out[1] = H2_PEER_RTP_OPUS_PAYLOAD_TYPE;
    h2_peer_write_be16(&out[2], sequence);
    h2_peer_write_be32(&out[4], timestamp);
    h2_peer_write_be32(&out[8], ssrc);
    memcpy(&out[H2_PEER_RTP_HEADER_SIZE], opus, opus_len);
    *out_len = H2_PEER_RTP_HEADER_SIZE + opus_len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_peer_rtp_parse(
    const uint8_t *data,
    size_t len,
    h2_peer_rtp_packet_t *out_packet) {
    if (out_packet != NULL) {
        memset(out_packet, 0, sizeof(*out_packet));
    }
    if (data == NULL || out_packet == NULL || len < H2_PEER_RTP_HEADER_SIZE ||
        len > 1500u || (data[0] >> 6u) != 2u) {
        return H2_PAL_ERR_FORMAT;
    }

    size_t offset = H2_PEER_RTP_HEADER_SIZE;
    size_t csrc_size = (size_t)(data[0] & 0x0fu) * 4u;
    if (csrc_size > len - offset) {
        return H2_PAL_ERR_TRUNCATED;
    }
    offset += csrc_size;
    if ((data[0] & 0x10u) != 0u) {
        if (len - offset < 4u) {
            return H2_PAL_ERR_TRUNCATED;
        }
        size_t extension_size = (size_t)h2_peer_read_be16(&data[offset + 2u]) * 4u;
        offset += 4u;
        if (extension_size > len - offset) {
            return H2_PAL_ERR_TRUNCATED;
        }
        offset += extension_size;
    }
    size_t payload_len = len - offset;
    if ((data[0] & 0x20u) != 0u) {
        if (payload_len == 0u || data[len - 1u] == 0u ||
            (size_t)data[len - 1u] > payload_len) {
            return H2_PAL_ERR_FORMAT;
        }
        payload_len -= data[len - 1u];
    }
    if (payload_len == 0u) {
        return H2_PAL_ERR_FORMAT;
    }

    out_packet->payload_type = data[1] & 0x7fu;
    out_packet->sequence = h2_peer_read_be16(&data[2]);
    out_packet->timestamp = h2_peer_read_be32(&data[4]);
    out_packet->ssrc = h2_peer_read_be32(&data[8]);
    out_packet->payload = &data[offset];
    out_packet->payload_len = payload_len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_peer_rtcp_validate(const uint8_t *data, size_t len) {
    if (data == NULL || len < 4u || len > 1500u || (data[0] >> 6u) != 2u) {
        return H2_PAL_ERR_FORMAT;
    }
    size_t offset = 0u;
    while (offset < len) {
        if (len - offset < 4u || (data[offset] >> 6u) != 2u ||
            data[offset + 1u] < 192u ||
            data[offset + 1u] > 223u) {
            return H2_PAL_ERR_FORMAT;
        }
        size_t packet_size = ((size_t)h2_peer_read_be16(&data[offset + 2u]) + 1u) * 4u;
        if (packet_size < 4u || packet_size > len - offset) {
            return H2_PAL_ERR_TRUNCATED;
        }
        offset += packet_size;
    }
    return H2_PAL_OK;
}
