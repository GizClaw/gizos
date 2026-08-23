#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_webrtc_peer_create(void *p0, const h2_pal_webrtc_callbacks_t *p1, h2_pal_webrtc_peer_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_webrtc_peer_start_offer(h2_pal_webrtc_peer_t *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_webrtc_peer_add_ice_server(
    h2_pal_webrtc_peer_t *p0,
    const h2_pal_webrtc_ice_server_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_webrtc_peer_set_remote_sdp(h2_pal_webrtc_peer_t *p0, h2_pal_webrtc_sdp_type_t p1, h2_pal_webrtc_str_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_webrtc_peer_create_data_channel(h2_pal_webrtc_peer_t *p0, const h2_pal_webrtc_channel_config_t *p1, h2_pal_webrtc_channel_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_webrtc_peer_poll(h2_pal_webrtc_peer_t *p0, int p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_webrtc_peer_send_opus(
    h2_pal_webrtc_peer_t *p0,
    const uint8_t *p1,
    size_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_webrtc_channel_send(h2_pal_webrtc_channel_t *p0, const uint8_t *p1, size_t p2, int p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_webrtc_channel_close(h2_pal_webrtc_channel_t *p0) {
    (void)p0;
}

static void unsupported_webrtc_peer_close(h2_pal_webrtc_peer_t *p0) {
    (void)p0;
}

static const h2_pal_webrtc_vtable_t unsupported_webrtc_vtable = {
    .peer_create = unsupported_webrtc_peer_create,
    .peer_add_ice_server = unsupported_webrtc_peer_add_ice_server,
    .peer_start_offer = unsupported_webrtc_peer_start_offer,
    .peer_set_remote_sdp = unsupported_webrtc_peer_set_remote_sdp,
    .peer_create_data_channel = unsupported_webrtc_peer_create_data_channel,
    .peer_poll = unsupported_webrtc_peer_poll,
    .peer_send_opus = unsupported_webrtc_peer_send_opus,
    .channel_send = unsupported_webrtc_channel_send,
    .channel_close = unsupported_webrtc_channel_close,
    .peer_close = unsupported_webrtc_peer_close,
};
static const h2_pal_webrtc_api_t unsupported_webrtc_api = { .user = NULL, .vtable = &unsupported_webrtc_vtable };
const h2_pal_webrtc_api_t *h2_pal_unsupported_webrtc_api(void) { return &unsupported_webrtc_api; }
