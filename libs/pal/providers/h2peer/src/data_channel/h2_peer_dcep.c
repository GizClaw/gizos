#include "h2_peer_dcep.h"

#include <string.h>

#define H2_PEER_DCEP_OPEN 0x03u
#define H2_PEER_DCEP_RELIABLE 0x00u
#define H2_PEER_DCEP_RELIABLE_UNORDERED 0x80u
#define H2_PEER_DCEP_PARTIAL_REXMIT 0x01u
#define H2_PEER_DCEP_PARTIAL_REXMIT_UNORDERED 0x81u
#define H2_PEER_DCEP_HEADER_SIZE 12u

static uint16_t h2_peer_dcep_read_be16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8u) | (uint16_t)data[1]);
}

static void h2_peer_dcep_write_be16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8u);
    data[1] = (uint8_t)value;
}

h2_pal_result_t h2_peer_dcep_write_open(
    const uint8_t *label,
    size_t label_len,
    int ordered,
    int reliable,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (label == NULL || label_len == 0u || label_len > H2_PEER_DCEP_LABEL_MAX ||
        out == NULL || out_len == NULL ||
        out_cap < H2_PEER_DCEP_HEADER_SIZE + label_len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out[0] = H2_PEER_DCEP_OPEN;
    out[1] = reliable ? (ordered ? H2_PEER_DCEP_RELIABLE : H2_PEER_DCEP_RELIABLE_UNORDERED)
                      : (ordered ? H2_PEER_DCEP_PARTIAL_REXMIT : H2_PEER_DCEP_PARTIAL_REXMIT_UNORDERED);
    memset(&out[2], 0, 6u);
    h2_peer_dcep_write_be16(&out[8], (uint16_t)label_len);
    h2_peer_dcep_write_be16(&out[10], 0u);
    memcpy(&out[12], label, label_len);
    *out_len = H2_PEER_DCEP_HEADER_SIZE + label_len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_peer_dcep_parse_open(
    const uint8_t *data,
    size_t len,
    h2_peer_dcep_open_t *out_open) {
    if (out_open != NULL) {
        memset(out_open, 0, sizeof(*out_open));
    }
    if (data == NULL || out_open == NULL || len < H2_PEER_DCEP_HEADER_SIZE ||
        data[0] != H2_PEER_DCEP_OPEN) {
        return H2_PAL_ERR_FORMAT;
    }
    uint8_t type = data[1];
    if (type != H2_PEER_DCEP_RELIABLE && type != H2_PEER_DCEP_RELIABLE_UNORDERED &&
        type != H2_PEER_DCEP_PARTIAL_REXMIT &&
        type != H2_PEER_DCEP_PARTIAL_REXMIT_UNORDERED) {
        return H2_PAL_ERR_FORMAT;
    }
    size_t label_len = h2_peer_dcep_read_be16(&data[8]);
    size_t protocol_len = h2_peer_dcep_read_be16(&data[10]);
    if (label_len == 0u || label_len > H2_PEER_DCEP_LABEL_MAX ||
        label_len > len - H2_PEER_DCEP_HEADER_SIZE ||
        protocol_len > len - H2_PEER_DCEP_HEADER_SIZE - label_len ||
        H2_PEER_DCEP_HEADER_SIZE + label_len + protocol_len != len) {
        return H2_PAL_ERR_TRUNCATED;
    }
    out_open->ordered = (type & 0x80u) == 0u;
    out_open->reliable = (type & 0x01u) == 0u;
    out_open->label = &data[H2_PEER_DCEP_HEADER_SIZE];
    out_open->label_len = label_len;
    return H2_PAL_OK;
}
