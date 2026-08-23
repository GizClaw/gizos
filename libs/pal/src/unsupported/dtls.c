#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_dtls_session_create(
    void *user, const h2_pal_dtls_session_config_t *config,
    h2_pal_dtls_session_t **out_session) {
    (void)user;
    (void)config;
    if (out_session != NULL) {
        *out_session = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_dtls_fingerprint(
    void *user, h2_pal_dtls_session_t *session,
    uint8_t out[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    (void)user;
    (void)session;
    if (out != NULL) {
        memset(out, 0, H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE);
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_dtls_set_fingerprint(
    void *user, h2_pal_dtls_session_t *session,
    const uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    (void)user;
    (void)session;
    (void)fingerprint;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_dtls_handshake(
    void *user, h2_pal_dtls_session_t *session,
    const uint8_t *datagram, size_t datagram_len,
    uint64_t now_ms, uint64_t deadline_ms, int *out_complete) {
    (void)user;
    (void)session;
    (void)datagram;
    (void)datagram_len;
    (void)now_ms;
    (void)deadline_ms;
    if (out_complete != NULL) {
        *out_complete = 0;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_dtls_deadline(
    void *user, h2_pal_dtls_session_t *session, uint64_t *out_deadline_ms) {
    (void)user;
    (void)session;
    if (out_deadline_ms != NULL) {
        *out_deadline_ms = 0u;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_dtls_session_only(
    void *user, h2_pal_dtls_session_t *session) {
    (void)user;
    (void)session;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_dtls_profile(
    void *user, h2_pal_dtls_session_t *session,
    h2_pal_dtls_srtp_profile_t *out_profile) {
    (void)user;
    (void)session;
    if (out_profile != NULL) {
        *out_profile = (h2_pal_dtls_srtp_profile_t)0;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_dtls_export(
    void *user, h2_pal_dtls_session_t *session,
    uint8_t *out, size_t out_len) {
    (void)user;
    (void)session;
    if (out != NULL) {
        memset(out, 0, out_len);
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_dtls_write(
    void *user, h2_pal_dtls_session_t *session,
    const uint8_t *data, size_t data_len) {
    (void)user;
    (void)session;
    (void)data;
    (void)data_len;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_dtls_destroy(
    void *user, h2_pal_dtls_session_t **session) {
    (void)user;
    if (session != NULL) {
        *session = NULL;
    }
}

static const h2_pal_dtls_vtable_t unsupported_dtls_vtable = {
    .session_create = unsupported_dtls_session_create,
    .session_get_local_fingerprint = unsupported_dtls_fingerprint,
    .session_set_remote_fingerprint = unsupported_dtls_set_fingerprint,
    .session_handshake = unsupported_dtls_handshake,
    .session_next_deadline_ms = unsupported_dtls_deadline,
    .session_flush = unsupported_dtls_session_only,
    .session_get_srtp_profile = unsupported_dtls_profile,
    .session_export_srtp_keying_material = unsupported_dtls_export,
    .session_write = unsupported_dtls_write,
    .session_consume_datagram = unsupported_dtls_write,
    .session_close = unsupported_dtls_session_only,
    .session_destroy = unsupported_dtls_destroy,
};

static const h2_pal_dtls_api_t unsupported_dtls_api = {
    .user = NULL,
    .vtable = &unsupported_dtls_vtable,
};

const h2_pal_dtls_api_t *h2_pal_unsupported_dtls_api(void) {
    return &unsupported_dtls_api;
}
