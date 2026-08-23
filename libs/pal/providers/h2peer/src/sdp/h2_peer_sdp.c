#include "h2_peer_sdp.h"

#include <stdio.h>
#include <string.h>

static int h2_peer_sdp_line_equals(
    const char *line,
    size_t line_len,
    const char *expected) {
    size_t expected_len = strlen(expected);
    return line_len == expected_len && memcmp(line, expected, line_len) == 0;
}

static int h2_peer_sdp_line_starts_with(
    const char *line,
    size_t line_len,
    const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return line_len >= prefix_len && memcmp(line, prefix, prefix_len) == 0;
}

static int h2_peer_sdp_line_has_value(
    const char *line,
    size_t line_len,
    const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return line_len > prefix_len && memcmp(line, prefix, prefix_len) == 0;
}

h2_pal_result_t h2_peer_sdp_parse(
    h2_pal_webrtc_str_t sdp,
    h2_peer_sdp_description_t *out_description) {
    if (out_description != NULL) {
        memset(out_description, 0, sizeof(*out_description));
    }
    if (sdp.data == NULL || out_description == NULL || sdp.len == 0u ||
        sdp.len > 4096u) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    size_t offset = 0u;
    while (offset < sdp.len) {
        size_t end = offset;
        while (end < sdp.len && sdp.data[end] != '\n') {
            end++;
        }
        size_t line_len = end - offset;
        if (line_len != 0u && sdp.data[offset + line_len - 1u] == '\r') {
            line_len--;
        }
        if (line_len == 0u || line_len > H2_PEER_SDP_LINE_MAX) {
            return H2_PAL_ERR_FORMAT;
        }
        const char *line = &sdp.data[offset];
        out_description->has_version |= h2_peer_sdp_line_equals(line, line_len, "v=0");
        out_description->has_ice_ufrag |= h2_peer_sdp_line_has_value(line, line_len, "a=ice-ufrag:");
        out_description->has_ice_pwd |= h2_peer_sdp_line_has_value(line, line_len, "a=ice-pwd:");
        static const char fingerprint_prefix[] = "a=fingerprint:";
        if (h2_peer_sdp_line_has_value(line, line_len, fingerprint_prefix)) {
            out_description->has_fingerprint = 1;
            out_description->fingerprint.data = line + sizeof(fingerprint_prefix) - 1u;
            out_description->fingerprint.len = line_len - (sizeof(fingerprint_prefix) - 1u);
        }
        out_description->has_opus |= h2_peer_sdp_line_starts_with(line, line_len, "a=rtpmap:111 opus/48000/2");
        out_description->has_data_channel |= h2_peer_sdp_line_starts_with(line, line_len, "m=application ");
        offset = end < sdp.len ? end + 1u : end;
    }
    if (!out_description->has_version || !out_description->has_ice_ufrag ||
        !out_description->has_ice_pwd || !out_description->has_fingerprint) {
        memset(out_description, 0, sizeof(*out_description));
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_peer_sdp_write_offer(
    const uint8_t random[16],
    uint32_t ssrc,
    h2_pal_webrtc_str_t fingerprint,
    char *out,
    size_t out_cap,
    size_t *out_len) {
    static const char hex[] = "0123456789abcdef";
    char ufrag[9];
    char pwd[25];
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (random == NULL || fingerprint.data == NULL || fingerprint.len == 0u ||
        fingerprint.len > 95u || out == NULL || out_cap == 0u || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < 4u; ++i) {
        ufrag[i * 2u] = hex[random[i] >> 4u];
        ufrag[i * 2u + 1u] = hex[random[i] & 0x0fu];
    }
    ufrag[8] = '\0';
    for (size_t i = 0u; i < 12u; ++i) {
        pwd[i * 2u] = hex[random[i + 4u] >> 4u];
        pwd[i * 2u + 1u] = hex[random[i + 4u] & 0x0fu];
    }
    pwd[24] = '\0';
    int written = snprintf(
        out,
        out_cap,
        "v=0\r\n"
        "o=- 0 0 IN IP4 0.0.0.0\r\n"
        "s=H2Peer\r\n"
        "t=0 0\r\n"
        "a=ice-ufrag:%s\r\n"
        "a=ice-pwd:%s\r\n"
        "a=fingerprint:sha-256 %.*s\r\n"
        "a=setup:actpass\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=ssrc:%lu cname:h2peer\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "a=sctp-port:5000\r\n",
        ufrag,
        pwd,
        (int)fingerprint.len,
        fingerprint.data,
        (unsigned long)ssrc);
    if (written < 0 || (size_t)written >= out_cap) {
        return H2_PAL_ERR_NO_SPACE;
    }
    *out_len = (size_t)written;
    return H2_PAL_OK;
}
