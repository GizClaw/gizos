#ifndef H2_PEER_SDP_H
#define H2_PEER_SDP_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/application/h2_pal_webrtc.h"

#include <stddef.h>
#include <stdint.h>

#define H2_PEER_SDP_LINE_MAX 256u

typedef struct h2_peer_sdp_description {
    int has_version;
    int has_ice_ufrag;
    int has_ice_pwd;
    int has_fingerprint;
    h2_pal_webrtc_str_t fingerprint;
    int has_opus;
    int has_data_channel;
} h2_peer_sdp_description_t;

h2_pal_result_t h2_peer_sdp_parse(
    h2_pal_webrtc_str_t sdp,
    h2_peer_sdp_description_t *out_description);

h2_pal_result_t h2_peer_sdp_write_offer(
    const uint8_t random[16],
    uint32_t ssrc,
    h2_pal_webrtc_str_t fingerprint,
    char *out,
    size_t out_cap,
    size_t *out_len);

#endif
