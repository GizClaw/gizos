#include "h2_peer_providers.h"

static h2_pal_result_t h2_peer_provider_unavailable_open(
    void *user,
    void **out_session) {
    (void)user;
    if (out_session != NULL) {
        *out_session = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_peer_ice_unavailable_open(
    void *user,
    const h2_pal_webrtc_ice_server_t *servers,
    size_t server_count,
    void **out_session) {
    (void)servers;
    (void)server_count;
    return h2_peer_provider_unavailable_open(user, out_session);
}

static h2_pal_result_t h2_peer_provider_unavailable_poll(
    void *user,
    void *session,
    int timeout_ms) {
    (void)user;
    (void)session;
    (void)timeout_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_peer_dtls_unavailable_fingerprint(
    void *user,
    void *session,
    char *out,
    size_t out_cap,
    size_t *out_len) {
    (void)user;
    (void)session;
    (void)out;
    (void)out_cap;
    if (out_len != NULL) {
        *out_len = 0u;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_peer_dtls_unavailable_set_fingerprint(
    void *user,
    void *session,
    h2_pal_webrtc_str_t fingerprint) {
    (void)user;
    (void)session;
    (void)fingerprint;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_peer_sctp_unavailable_poll(
    void *user,
    void *session,
    int timeout_ms,
    h2_peer_sctp_event_fn event_fn,
    void *event_user) {
    (void)event_fn;
    (void)event_user;
    return h2_peer_provider_unavailable_poll(user, session, timeout_ms);
}

static h2_pal_result_t h2_peer_srtp_unavailable_send(
    void *user,
    void *session,
    const uint8_t *packet,
    size_t packet_len) {
    (void)user;
    (void)session;
    (void)packet;
    (void)packet_len;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_peer_srtp_unavailable_receive(
    void *user,
    void *session,
    const uint8_t *packet,
    size_t packet_len,
    uint8_t *out_packet,
    size_t out_cap,
    size_t *out_len) {
    (void)user;
    (void)session;
    (void)packet;
    (void)packet_len;
    (void)out_packet;
    (void)out_cap;
    if (out_len != NULL) {
        *out_len = 0u;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_peer_sctp_unavailable_channel_open(
    void *user,
    void *session,
    uint16_t stream_id,
    h2_pal_webrtc_str_t label,
    int ordered,
    int reliable) {
    (void)user;
    (void)session;
    (void)stream_id;
    (void)label;
    (void)ordered;
    (void)reliable;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t h2_peer_sctp_unavailable_send(
    void *user,
    void *session,
    uint16_t stream_id,
    const uint8_t *data,
    size_t len,
    int is_text) {
    (void)stream_id;
    (void)data;
    (void)len;
    (void)is_text;
    return h2_peer_srtp_unavailable_send(user, session, data, len);
}

static h2_pal_result_t h2_peer_sctp_unavailable_channel_close(
    void *user,
    void *session,
    uint16_t stream_id) {
    (void)user;
    (void)session;
    (void)stream_id;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void h2_peer_provider_unavailable_close(void *user, void *session) {
    (void)user;
    (void)session;
}

static const h2_peer_dtls_provider_vtable_t h2_peer_unavailable_dtls_vtable = {
    .open = h2_peer_provider_unavailable_open,
    .get_local_fingerprint = h2_peer_dtls_unavailable_fingerprint,
    .set_remote_fingerprint = h2_peer_dtls_unavailable_set_fingerprint,
    .poll = h2_peer_provider_unavailable_poll,
    .close = h2_peer_provider_unavailable_close,
};

static const h2_peer_ice_provider_vtable_t h2_peer_unavailable_ice_vtable = {
    .open = h2_peer_ice_unavailable_open,
    .poll = h2_peer_provider_unavailable_poll,
    .close = h2_peer_provider_unavailable_close,
};

static const h2_peer_srtp_provider_vtable_t h2_peer_unavailable_srtp_vtable = {
    .open = h2_peer_provider_unavailable_open,
    .send_rtp = h2_peer_srtp_unavailable_send,
    .receive_rtp = h2_peer_srtp_unavailable_receive,
    .close = h2_peer_provider_unavailable_close,
};

static const h2_peer_sctp_provider_vtable_t h2_peer_unavailable_sctp_vtable = {
    .open = h2_peer_provider_unavailable_open,
    .poll = h2_peer_sctp_unavailable_poll,
    .channel_open = h2_peer_sctp_unavailable_channel_open,
    .send = h2_peer_sctp_unavailable_send,
    .channel_close = h2_peer_sctp_unavailable_channel_close,
    .close = h2_peer_provider_unavailable_close,
};

static const h2_peer_provider_bundle_t h2_peer_unavailable_bundle = {
    .ice = {.user = NULL, .vtable = &h2_peer_unavailable_ice_vtable},
    .dtls = {.user = NULL, .vtable = &h2_peer_unavailable_dtls_vtable},
    .srtp = {.user = NULL, .vtable = &h2_peer_unavailable_srtp_vtable},
    .sctp = {.user = NULL, .vtable = &h2_peer_unavailable_sctp_vtable},
};

const h2_peer_provider_bundle_t *h2_peer_unavailable_providers(void) {
    return &h2_peer_unavailable_bundle;
}
