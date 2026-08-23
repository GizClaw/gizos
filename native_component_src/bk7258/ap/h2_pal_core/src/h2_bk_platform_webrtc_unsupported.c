#include "h2_bk_platform_core.h"

static h2_pal_result_t h2_bk_platform_webrtc_peer_create(
    void *user,
    const h2_pal_webrtc_callbacks_t *callbacks,
    h2_pal_webrtc_peer_t **out_peer) {
    (void)user;
    (void)callbacks;
    if (out_peer != NULL) {
        *out_peer = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_platform_webrtc_peer_start_offer(h2_pal_webrtc_peer_t *peer) {
    (void)peer;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_platform_webrtc_peer_set_remote_sdp(
    h2_pal_webrtc_peer_t *peer,
    h2_pal_webrtc_sdp_type_t type,
    h2_pal_webrtc_str_t sdp) {
    (void)peer;
    (void)type;
    (void)sdp;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_platform_webrtc_peer_create_data_channel(
    h2_pal_webrtc_peer_t *peer,
    const h2_pal_webrtc_channel_config_t *config,
    h2_pal_webrtc_channel_t **out_channel) {
    (void)peer;
    (void)config;
    if (out_channel != NULL) {
        *out_channel = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_platform_webrtc_peer_poll(h2_pal_webrtc_peer_t *peer, int timeout_ms) {
    (void)peer;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_platform_webrtc_peer_send_opus(
    h2_pal_webrtc_peer_t *peer,
    const uint8_t *opus,
    size_t opus_len) {
    (void)peer;
    (void)opus;
    (void)opus_len;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_bk_platform_webrtc_channel_send(
    h2_pal_webrtc_channel_t *channel,
    const uint8_t *data,
    size_t len,
    int is_text) {
    (void)channel;
    (void)data;
    (void)len;
    (void)is_text;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void h2_bk_platform_webrtc_channel_close(h2_pal_webrtc_channel_t *channel) {
    (void)channel;
}

static void h2_bk_platform_webrtc_peer_close(h2_pal_webrtc_peer_t *peer) {
    (void)peer;
}

const h2_pal_webrtc_api_t *h2_bk_platform_webrtc_api(void) {
    static const h2_pal_webrtc_vtable_t vtable = {
        .peer_create = h2_bk_platform_webrtc_peer_create,
        .peer_start_offer = h2_bk_platform_webrtc_peer_start_offer,
        .peer_set_remote_sdp = h2_bk_platform_webrtc_peer_set_remote_sdp,
        .peer_create_data_channel = h2_bk_platform_webrtc_peer_create_data_channel,
        .peer_poll = h2_bk_platform_webrtc_peer_poll,
        .peer_send_opus = h2_bk_platform_webrtc_peer_send_opus,
        .channel_send = h2_bk_platform_webrtc_channel_send,
        .channel_close = h2_bk_platform_webrtc_channel_close,
        .peer_close = h2_bk_platform_webrtc_peer_close,
    };
    static const h2_pal_webrtc_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
