#ifndef H2_PEER_RTP_H
#define H2_PEER_RTP_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#define H2_PEER_RTP_HEADER_SIZE 12u
#define H2_PEER_RTP_OPUS_PAYLOAD_TYPE 111u

typedef struct h2_peer_rtp_packet {
    uint8_t payload_type;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    const uint8_t *payload;
    size_t payload_len;
} h2_peer_rtp_packet_t;

h2_pal_result_t h2_peer_rtp_write_opus(
    uint16_t sequence,
    uint32_t timestamp,
    uint32_t ssrc,
    const uint8_t *opus,
    size_t opus_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len);

h2_pal_result_t h2_peer_rtp_parse(
    const uint8_t *data,
    size_t len,
    h2_peer_rtp_packet_t *out_packet);

h2_pal_result_t h2_peer_rtcp_validate(const uint8_t *data, size_t len);

#endif
