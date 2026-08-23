#include "h2_wolfssl_internal.h"

#include <limits.h>
#include <string.h>

#include <wolfssl/wolfio.h>

#define H2_WOLFSSL_SRTP_EXPORTER_LABEL "EXTRACTOR-dtls_srtp"

static int h2_wolfssl_dtls_defer_certificate_verification(
    int preverify, WOLFSSL_X509_STORE_CTX *store) {
    (void)preverify;
    (void)store;
    return 1;
}

static void h2_wolfssl_dtls_destroy_impl(
    h2_pal_dtls_session_t *session) {
    if (session == NULL) {
        return;
    }
    if (session->ssl != NULL) {
        wolfSSL_free(session->ssl);
    }
    if (session->context != NULL) {
        wolfSSL_CTX_free(session->context);
    }
    if (session->plaintext != NULL) {
        memset(session->plaintext, 0, session->config.max_plaintext_size);
    }
    if (session->pending_bytes != NULL) {
        memset(
            session->pending_bytes, 0,
            session->config.max_pending_output_bytes);
    }
    h2_wolfssl_free(session->plaintext);
    h2_wolfssl_free(session->pending_lengths);
    h2_wolfssl_free(session->pending_bytes);
    memset(session, 0, sizeof(*session));
    h2_wolfssl_free(session);
    h2_wolfssl_session_release();
}

static h2_pal_result_t h2_wolfssl_dtls_session_create(
    void *user,
    const h2_pal_dtls_session_config_t *config,
    h2_pal_dtls_session_t **out_session) {
    h2_pal_dtls_session_t *session;
    WOLFSSL_METHOD *method;
    h2_pal_result_t result;
    (void)user;

    if (!h2_wolfssl_is_ready()) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->max_datagram_size > (size_t)USHRT_MAX ||
        config->max_plaintext_size > (size_t)INT_MAX ||
        config->max_pending_output_bytes >
            SIZE_MAX / sizeof(*session->pending_lengths)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    result = h2_wolfssl_session_acquire();
    if (result != H2_PAL_OK) {
        return result;
    }
    session = h2_wolfssl_alloc(sizeof(*session));
    if (session == NULL) {
        h2_wolfssl_session_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(*session));
    session->config = *config;
    session->pending_bytes = h2_wolfssl_alloc(
        config->max_pending_output_bytes);
    session->pending_lengths = h2_wolfssl_alloc(
        config->max_pending_output_bytes * sizeof(*session->pending_lengths));
    session->plaintext = h2_wolfssl_alloc(config->max_plaintext_size);
    if (session->pending_bytes == NULL || session->pending_lengths == NULL ||
        session->plaintext == NULL) {
        h2_wolfssl_dtls_destroy_impl(session);
        return H2_PAL_ERR_NO_MEMORY;
    }

    method = config->role == H2_PAL_DTLS_ROLE_SERVER
                 ? wolfDTLSv1_2_server_method()
                 : wolfDTLSv1_2_client_method();
    session->context = method == NULL ? NULL : wolfSSL_CTX_new(method);
    if (session->context == NULL) {
        h2_wolfssl_dtls_destroy_impl(session);
        return H2_PAL_ERR_NO_MEMORY;
    }
    wolfSSL_CTX_set_verify(
        session->context,
        WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT,
        h2_wolfssl_dtls_defer_certificate_verification);
    wolfSSL_CTX_SetIOSend(session->context, h2_wolfssl_dtls_send);
    wolfSSL_CTX_SetIORecv(session->context, h2_wolfssl_dtls_recv);
    if (wolfSSL_CTX_dtls_set_mtu(
            session->context,
            (unsigned short)config->max_datagram_size) != WOLFSSL_SUCCESS ||
        wolfSSL_CTX_set_tlsext_use_srtp(
            session->context, "SRTP_AES128_CM_SHA1_80") != 0 ||
        h2_wolfssl_dtls_generate_identity(
            session->context,
            session->local_fingerprint) != H2_PAL_OK) {
        h2_wolfssl_dtls_destroy_impl(session);
        return H2_PAL_ERR_IO;
    }
    session->ssl = wolfSSL_new(session->context);
    if (session->ssl == NULL) {
        h2_wolfssl_dtls_destroy_impl(session);
        return H2_PAL_ERR_NO_MEMORY;
    }
    wolfSSL_SetIOReadCtx(session->ssl, session);
    wolfSSL_SetIOWriteCtx(session->ssl, session);
    wolfSSL_dtls_set_using_nonblock(session->ssl, 1);
    wolfSSL_KeepArrays(session->ssl);
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfssl_dtls_local_fingerprint(
    void *user,
    h2_pal_dtls_session_t *session,
    uint8_t out_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    (void)user;
    memcpy(
        out_fingerprint, session->local_fingerprint,
        H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfssl_dtls_remote_fingerprint(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    (void)user;
    if (session->handshake_complete || session->terminal_error != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->handshake_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->remote_fingerprint_set) {
        return memcmp(
                   session->remote_fingerprint, fingerprint,
                   H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE) == 0
                   ? H2_PAL_OK
                   : H2_PAL_ERR_INVALID_STATE;
    }
    memcpy(
        session->remote_fingerprint, fingerprint,
        H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE);
    session->remote_fingerprint_set = 1;
    return H2_PAL_OK;
}

static void h2_wolfssl_dtls_schedule_timeout(
    h2_pal_dtls_session_t *session,
    uint64_t now_ms) {
    int timeout_seconds = wolfSSL_dtls_get_current_timeout(session->ssl);
    uint64_t timeout_ms;
    if (timeout_seconds <= 0) {
        session->next_deadline_ms = session->handshake_deadline_ms;
        return;
    }
    timeout_ms = (uint64_t)(unsigned int)timeout_seconds * 1000u;
    session->next_deadline_ms = now_ms > UINT64_MAX - timeout_ms
                                    ? UINT64_MAX
                                    : now_ms + timeout_ms;
    if (session->next_deadline_ms > session->handshake_deadline_ms) {
        session->next_deadline_ms = session->handshake_deadline_ms;
    }
}

static h2_pal_result_t h2_wolfssl_dtls_fail(
    h2_pal_dtls_session_t *session,
    h2_pal_result_t result) {
    session->terminal_error = result;
    return result;
}

static h2_pal_result_t h2_wolfssl_dtls_handshake(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len,
    uint64_t now_ms,
    uint64_t deadline_ms,
    int *out_complete) {
    h2_pal_result_t flush_result;
    int wolf_result;
    int wolf_error;
    (void)user;

    if (session->terminal_error != H2_PAL_OK) {
        return session->terminal_error;
    }
    if (!session->remote_fingerprint_set || session->close_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->handshake_started &&
        deadline_ms != session->handshake_deadline_ms) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (datagram_len > session->config.max_datagram_size) {
        return h2_wolfssl_dtls_fail(session, H2_PAL_ERR_TRUNCATED);
    }
    if (session->handshake_complete) {
        *out_complete = 1;
        return H2_PAL_OK;
    }
    if (!session->handshake_started) {
        session->handshake_started = 1;
        session->handshake_deadline_ms = deadline_ms;
    }
    if (now_ms >= session->handshake_deadline_ms) {
        return h2_wolfssl_dtls_fail(session, H2_PAL_ERR_TIMEOUT);
    }
    flush_result = h2_wolfssl_dtls_flush_pending(session);
    if (flush_result != H2_PAL_OK) {
        return flush_result;
    }

    if (datagram_len == 0u && session->next_deadline_ms != 0u &&
        now_ms < session->next_deadline_ms) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (datagram_len == 0u && session->next_deadline_ms != 0u &&
        now_ms >= session->next_deadline_ms) {
        wolf_result = wolfSSL_dtls_got_timeout(session->ssl);
    } else {
        h2_wolfssl_dtls_set_input(session, datagram, datagram_len);
        wolf_result = session->config.role == H2_PAL_DTLS_ROLE_SERVER
                          ? wolfSSL_accept(session->ssl)
                          : wolfSSL_connect(session->ssl);
        h2_wolfssl_dtls_clear_input(session);
    }
    if (session->terminal_error != H2_PAL_OK) {
        return session->terminal_error;
    }
    if (wolf_result == WOLFSSL_SUCCESS) {
        h2_pal_result_t verify_result = h2_wolfssl_dtls_verify_peer(session);
        if (verify_result != H2_PAL_OK) {
            return h2_wolfssl_dtls_fail(session, verify_result);
        }
        session->handshake_complete = 1;
        session->next_deadline_ms = 0u;
        *out_complete = 1;
        return H2_PAL_OK;
    }
    wolf_error = wolfSSL_get_error(session->ssl, wolf_result);
    if (wolf_error != WOLFSSL_ERROR_WANT_READ &&
        wolf_error != WOLFSSL_ERROR_WANT_WRITE) {
        return h2_wolfssl_dtls_fail(session, H2_PAL_ERR_IO);
    }
    h2_wolfssl_dtls_schedule_timeout(session, now_ms);
    return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t h2_wolfssl_dtls_next_deadline(
    void *user,
    h2_pal_dtls_session_t *session,
    uint64_t *out_deadline_ms) {
    (void)user;
    if (session->terminal_error != H2_PAL_OK) {
        return session->terminal_error;
    }
    if (!session->handshake_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->handshake_complete) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_deadline_ms = session->next_deadline_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfssl_dtls_flush(
    void *user,
    h2_pal_dtls_session_t *session) {
    (void)user;
    if (session->terminal_error != H2_PAL_OK) {
        return session->terminal_error;
    }
    return h2_wolfssl_dtls_flush_pending(session);
}

static h2_pal_result_t h2_wolfssl_dtls_srtp_profile(
    void *user,
    h2_pal_dtls_session_t *session,
    h2_pal_dtls_srtp_profile_t *out_profile) {
    const WOLFSSL_SRTP_PROTECTION_PROFILE *profile;
    (void)user;
    if (!session->handshake_complete) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    profile = wolfSSL_get_selected_srtp_profile(session->ssl);
    if (profile == NULL || profile->id != SRTP_AES128_CM_SHA1_80) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_profile = H2_PAL_DTLS_SRTP_PROFILE_AES128_CM_SHA1_80;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfssl_dtls_export_srtp(
    void *user,
    h2_pal_dtls_session_t *session,
    uint8_t *out,
    size_t out_len) {
    (void)user;
    if (!session->handshake_complete) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return wolfSSL_export_keying_material(
               session->ssl, out, out_len,
               H2_WOLFSSL_SRTP_EXPORTER_LABEL,
               sizeof(H2_WOLFSSL_SRTP_EXPORTER_LABEL) - 1u,
               NULL, 0u, 0) == WOLFSSL_SUCCESS
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t h2_wolfssl_dtls_write(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *plaintext,
    size_t plaintext_len) {
    int result;
    (void)user;
    if (!session->handshake_complete || session->close_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->terminal_error != H2_PAL_OK) {
        return session->terminal_error;
    }
    if (plaintext_len > session->config.max_plaintext_size) {
        return H2_PAL_ERR_TRUNCATED;
    }
    if (session->pending_count != 0u) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    result = wolfSSL_write(session->ssl, plaintext, (int)plaintext_len);
    if (session->terminal_error != H2_PAL_OK) {
        return session->terminal_error;
    }
    if (result != (int)plaintext_len) {
        return h2_wolfssl_dtls_fail(session, H2_PAL_ERR_IO);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfssl_dtls_consume_datagram(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len) {
    int result;
    (void)user;
    if (!session->handshake_complete || session->close_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->terminal_error != H2_PAL_OK) {
        return session->terminal_error;
    }
    if (datagram_len > session->config.max_datagram_size) {
        return H2_PAL_ERR_TRUNCATED;
    }
    h2_wolfssl_dtls_set_input(session, datagram, datagram_len);
    for (;;) {
        h2_pal_result_t callback_result;
        int error;
        result = wolfSSL_read(
            session->ssl, session->plaintext,
            (int)session->config.max_plaintext_size);
        if (result > 0) {
            callback_result = session->config.plaintext(
                session->config.io_user, session->plaintext, (size_t)result);
            memset(session->plaintext, 0, (size_t)result);
            if (callback_result != H2_PAL_OK) {
                h2_wolfssl_dtls_clear_input(session);
                return h2_wolfssl_dtls_fail(session, callback_result);
            }
            continue;
        }
        error = wolfSSL_get_error(session->ssl, result);
        h2_wolfssl_dtls_clear_input(session);
        if (session->terminal_error != H2_PAL_OK) {
            return session->terminal_error;
        }
        if (error == WOLFSSL_ERROR_WANT_READ ||
            error == WOLFSSL_ERROR_WANT_WRITE) {
            return H2_PAL_OK;
        }
        if (result == 0) {
            return H2_PAL_ERR_CLOSED;
        }
        return h2_wolfssl_dtls_fail(session, H2_PAL_ERR_IO);
    }
}

static h2_pal_result_t h2_wolfssl_dtls_close(
    void *user,
    h2_pal_dtls_session_t *session) {
    int result;
    int error;
    (void)user;
    if (session->close_started) {
        return H2_PAL_OK;
    }
    session->close_started = 1;
    result = wolfSSL_shutdown(session->ssl);
    if (session->terminal_error != H2_PAL_OK) {
        return session->terminal_error;
    }
    if (result == WOLFSSL_SUCCESS || result == WOLFSSL_SHUTDOWN_NOT_DONE) {
        return H2_PAL_OK;
    }
    error = wolfSSL_get_error(session->ssl, result);
    return error == WOLFSSL_ERROR_WANT_READ ||
                   error == WOLFSSL_ERROR_WANT_WRITE
               ? H2_PAL_OK
               : h2_wolfssl_dtls_fail(session, H2_PAL_ERR_IO);
}

static void h2_wolfssl_dtls_destroy(
    void *user,
    h2_pal_dtls_session_t **session) {
    h2_pal_dtls_session_t *owned;
    (void)user;
    owned = *session;
    *session = NULL;
    h2_wolfssl_dtls_destroy_impl(owned);
}

static const h2_pal_dtls_vtable_t h2_wolfssl_dtls_vtable = {
    .session_create = h2_wolfssl_dtls_session_create,
    .session_get_local_fingerprint = h2_wolfssl_dtls_local_fingerprint,
    .session_set_remote_fingerprint = h2_wolfssl_dtls_remote_fingerprint,
    .session_handshake = h2_wolfssl_dtls_handshake,
    .session_next_deadline_ms = h2_wolfssl_dtls_next_deadline,
    .session_flush = h2_wolfssl_dtls_flush,
    .session_get_srtp_profile = h2_wolfssl_dtls_srtp_profile,
    .session_export_srtp_keying_material = h2_wolfssl_dtls_export_srtp,
    .session_write = h2_wolfssl_dtls_write,
    .session_consume_datagram = h2_wolfssl_dtls_consume_datagram,
    .session_close = h2_wolfssl_dtls_close,
    .session_destroy = h2_wolfssl_dtls_destroy,
};

const h2_pal_dtls_api_t h2_wolfssl_dtls_provider = {
    .user = NULL,
    .vtable = &h2_wolfssl_dtls_vtable,
};
