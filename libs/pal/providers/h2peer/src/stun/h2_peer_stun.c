#include "h2_peer_stun.h"

#include <string.h>

#define H2_PEER_STUN_MAGIC_COOKIE UINT32_C(0x2112a442)

static uint16_t h2_peer_stun_read_be16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8u) | (uint16_t)data[1]);
}

static uint32_t h2_peer_stun_read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
           ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

h2_pal_result_t h2_peer_stun_parse(
    const uint8_t *data,
    size_t len,
    h2_peer_stun_message_t *out_message) {
    if (out_message != NULL) {
        memset(out_message, 0, sizeof(*out_message));
    }
    if (data == NULL || out_message == NULL || len < H2_PEER_STUN_HEADER_SIZE ||
        (data[0] & 0xc0u) != 0u ||
        h2_peer_stun_read_be32(&data[4]) != H2_PEER_STUN_MAGIC_COOKIE) {
        return H2_PAL_ERR_FORMAT;
    }
    size_t body_len = h2_peer_stun_read_be16(&data[2]);
    if ((body_len & 3u) != 0u || body_len != len - H2_PEER_STUN_HEADER_SIZE) {
        return H2_PAL_ERR_TRUNCATED;
    }
    size_t offset = H2_PEER_STUN_HEADER_SIZE;
    size_t count = 0u;
    while (offset < len) {
        if (len - offset < 4u) {
            return H2_PAL_ERR_TRUNCATED;
        }
        size_t attribute_len = h2_peer_stun_read_be16(&data[offset + 2u]);
        if (attribute_len > H2_PEER_STUN_ATTRIBUTE_MAX) {
            return H2_PAL_ERR_NO_SPACE;
        }
        offset += 4u;
        if (attribute_len > len - offset) {
            return H2_PAL_ERR_TRUNCATED;
        }
        offset += attribute_len;
        size_t padding = (4u - (attribute_len & 3u)) & 3u;
        if (padding > len - offset) {
            return H2_PAL_ERR_TRUNCATED;
        }
        offset += padding;
        count++;
    }
    out_message->type = h2_peer_stun_read_be16(data);
    memcpy(out_message->transaction_id, &data[8], sizeof(out_message->transaction_id));
    out_message->attribute_count = count;
    return H2_PAL_OK;
}

h2_pal_result_t h2_peer_stun_write_binding_request(
    const uint8_t transaction_id[12],
    uint8_t *out,
    size_t out_cap,
    size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (transaction_id == NULL || out == NULL || out_len == NULL ||
        out_cap < H2_PEER_STUN_HEADER_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out[0] = 0x00u;
    out[1] = 0x01u;
    out[2] = 0x00u;
    out[3] = 0x00u;
    out[4] = 0x21u;
    out[5] = 0x12u;
    out[6] = 0xa4u;
    out[7] = 0x42u;
    memcpy(&out[8], transaction_id, 12u);
    *out_len = H2_PEER_STUN_HEADER_SIZE;
    return H2_PAL_OK;
}
