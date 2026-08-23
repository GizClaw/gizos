#include "h2_bk_platform_core.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <limits.h>
#include <string.h>

#include <mbedtls/build_info.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#if defined(MBEDTLS_SSL_PROTO_DTLS) && defined(MBEDTLS_SSL_DTLS_SRTP) && \
    defined(MBEDTLS_X509_CRT_WRITE_C)

#define H2_BK_DTLS_CERT_CAPACITY 1024u
#define H2_BK_DTLS_MASTER_SECRET_CAPACITY 48u
#define H2_BK_DTLS_SRTP_EXPORTER_LABEL "EXTRACTOR-dtls_srtp"

struct h2_pal_dtls_session {
    h2_pal_dtls_session_config_t config;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_config;
    mbedtls_x509_crt certificate;
    mbedtls_x509write_cert certificate_writer;
    mbedtls_pk_context private_key;
    uint8_t certificate_der[H2_BK_DTLS_CERT_CAPACITY];
    uint8_t local_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t remote_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t *plaintext;
    uint8_t master_secret[H2_BK_DTLS_MASTER_SECRET_CAPACITY];
    uint8_t randoms[64];
    const uint8_t *input;
    size_t input_len;
    size_t master_secret_len;
    uint64_t timer_start_ms;
    uint64_t now_ms;
    uint32_t timer_intermediate_ms;
    uint32_t timer_final_ms;
    mbedtls_tls_prf_types prf;
    int input_consumed;
    int remote_fingerprint_set;
    int handshake_started;
    int handshake_complete;
    int close_started;
    int exporter_ready;
};

static int h2_bk_dtls_random(void *user, unsigned char *out, size_t len) {
    (void)user;
    return h2_pal_crypto_random(h2_bk_platform_crypto_api(), out, len) ==
                   H2_PAL_OK
               ? 0
               : -1;
}

static int h2_bk_dtls_send(
    void *user, const unsigned char *data, size_t len) {
    h2_pal_dtls_session_t *session = (h2_pal_dtls_session_t *)user;
    h2_pal_result_t result = session->config.send(
        session->config.io_user, data, len);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return result == H2_PAL_OK ? (int)len : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int h2_bk_dtls_recv(
    void *user, unsigned char *out, size_t capacity) {
    h2_pal_dtls_session_t *session = (h2_pal_dtls_session_t *)user;
    if (session->input == NULL || session->input_consumed) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (session->input_len > capacity || session->input_len > INT_MAX) {
        return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    }
    memcpy(out, session->input, session->input_len);
    session->input_consumed = 1;
    return (int)session->input_len;
}

static void h2_bk_dtls_set_input(
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len) {
    session->input = datagram;
    session->input_len = datagram_len;
    session->input_consumed = 0;
}

static void h2_bk_dtls_clear_input(h2_pal_dtls_session_t *session) {
    session->input = NULL;
    session->input_len = 0u;
    session->input_consumed = 0;
}

static void h2_bk_dtls_timer_set(
    void *user, uint32_t intermediate_ms, uint32_t final_ms) {
    h2_pal_dtls_session_t *session = (h2_pal_dtls_session_t *)user;
    session->timer_start_ms = session->now_ms;
    session->timer_intermediate_ms = intermediate_ms;
    session->timer_final_ms = final_ms;
}

static int h2_bk_dtls_timer_get(void *user) {
    h2_pal_dtls_session_t *session = (h2_pal_dtls_session_t *)user;
    if (session->timer_final_ms == 0u) {
        return -1;
    }
    uint64_t elapsed = session->now_ms - session->timer_start_ms;
    if (elapsed >= session->timer_final_ms) {
        return 2;
    }
    return elapsed >= session->timer_intermediate_ms ? 1 : 0;
}

static int h2_bk_dtls_defer_verify(
    void *user, mbedtls_x509_crt *certificate, int depth, uint32_t *flags) {
    (void)user;
    (void)certificate;
    (void)depth;
    *flags = 0u;
    return 0;
}

static void h2_bk_dtls_export_keys(
    void *user,
    mbedtls_ssl_key_export_type type,
    const unsigned char *secret,
    size_t secret_len,
    const unsigned char client_random[32],
    const unsigned char server_random[32],
    mbedtls_tls_prf_types prf) {
    h2_pal_dtls_session_t *session = (h2_pal_dtls_session_t *)user;
    if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET ||
        secret_len > sizeof(session->master_secret)) {
        return;
    }
    memcpy(session->master_secret, secret, secret_len);
    session->master_secret_len = secret_len;
    memcpy(session->randoms, client_random, 32u);
    memcpy(session->randoms + 32u, server_random, 32u);
    session->prf = prf;
    session->exporter_ready = 1;
}

static h2_pal_result_t h2_bk_dtls_generate_identity(
    h2_pal_dtls_session_t *session) {
    const mbedtls_pk_info_t *key_info =
        mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
    if (key_info == NULL ||
        mbedtls_pk_setup(&session->private_key, key_info) != 0) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (mbedtls_ecp_gen_key(
            MBEDTLS_ECP_DP_SECP256R1,
            mbedtls_pk_ec(session->private_key),
            h2_bk_dtls_random, session) != 0) {
        return H2_PAL_ERR_IO;
    }

    uint8_t serial[16];
    if (h2_bk_dtls_random(session, serial, sizeof(serial)) != 0) {
        return H2_PAL_ERR_IO;
    }
    serial[0] = (uint8_t)((serial[0] & 0x7fu) | 0x01u);
    mbedtls_x509write_crt_set_version(
        &session->certificate_writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(
        &session->certificate_writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(
        &session->certificate_writer, &session->private_key);
    mbedtls_x509write_crt_set_issuer_key(
        &session->certificate_writer, &session->private_key);
    int result = mbedtls_x509write_crt_set_subject_name(
        &session->certificate_writer, "CN=h2peer");
    if (result == 0) {
        result = mbedtls_x509write_crt_set_issuer_name(
            &session->certificate_writer, "CN=h2peer");
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_serial_raw(
            &session->certificate_writer, serial, sizeof(serial));
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_validity(
            &session->certificate_writer,
            "20240101000000", "20491231235959");
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_der(
            &session->certificate_writer,
            session->certificate_der, sizeof(session->certificate_der),
            h2_bk_dtls_random, session);
    }
    if (result <= 0 || (size_t)result > sizeof(session->certificate_der)) {
        return H2_PAL_ERR_IO;
    }
    size_t certificate_len = (size_t)result;
    uint8_t *certificate = session->certificate_der +
                           sizeof(session->certificate_der) - certificate_len;
    if (mbedtls_x509_crt_parse_der(
            &session->certificate, certificate, certificate_len) != 0 ||
        mbedtls_sha256(
            certificate, certificate_len,
            session->local_fingerprint, 0) != 0) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static void h2_bk_dtls_destroy_impl(h2_pal_dtls_session_t *session) {
    if (session == NULL) {
        return;
    }
    mbedtls_ssl_free(&session->ssl);
    mbedtls_ssl_config_free(&session->ssl_config);
    mbedtls_x509_crt_free(&session->certificate);
    mbedtls_x509write_crt_free(&session->certificate_writer);
    mbedtls_pk_free(&session->private_key);
    h2_pal_mem_free(h2_bk_platform_default_allocator(), session->plaintext);
    memset(session, 0, sizeof(*session));
    h2_pal_mem_free(h2_bk_platform_default_allocator(), session);
}

static h2_pal_result_t h2_bk_dtls_create(
    void *user,
    const h2_pal_dtls_session_config_t *config,
    h2_pal_dtls_session_t **out_session) {
    (void)user;
    h2_pal_dtls_session_t *session = h2_pal_mem_alloc(
        h2_bk_platform_default_allocator(), sizeof(*session));
    if (session == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(*session));
    session->config = *config;
    session->plaintext = h2_pal_mem_alloc(
        h2_bk_platform_default_allocator(), config->max_plaintext_size);
    if (session->plaintext == NULL) {
        h2_bk_dtls_destroy_impl(session);
        return H2_PAL_ERR_NO_MEMORY;
    }
    mbedtls_ssl_init(&session->ssl);
    mbedtls_ssl_config_init(&session->ssl_config);
    mbedtls_x509_crt_init(&session->certificate);
    mbedtls_x509write_crt_init(&session->certificate_writer);
    mbedtls_pk_init(&session->private_key);

    h2_pal_result_t identity_result = h2_bk_dtls_generate_identity(session);
    int endpoint = config->role == H2_PAL_DTLS_ROLE_SERVER
                       ? MBEDTLS_SSL_IS_SERVER
                       : MBEDTLS_SSL_IS_CLIENT;
    int result = identity_result == H2_PAL_OK
                     ? mbedtls_ssl_config_defaults(
                           &session->ssl_config, endpoint,
                           MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                           MBEDTLS_SSL_PRESET_DEFAULT)
                     : -1;
    static const mbedtls_ssl_srtp_profile profiles[] = {
        MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
        MBEDTLS_TLS_SRTP_UNSET,
    };
    if (result == 0) {
        mbedtls_ssl_conf_rng(
            &session->ssl_config, h2_bk_dtls_random, session);
        mbedtls_ssl_conf_authmode(
            &session->ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_verify(
            &session->ssl_config, h2_bk_dtls_defer_verify, session);
        result = mbedtls_ssl_conf_own_cert(
            &session->ssl_config, &session->certificate,
            &session->private_key);
    }
    if (result == 0) {
        result = mbedtls_ssl_conf_dtls_srtp_protection_profiles(
            &session->ssl_config, profiles);
    }
    if (result == 0) {
        result = mbedtls_ssl_setup(&session->ssl, &session->ssl_config);
    }
    if (result == 0) {
        mbedtls_ssl_set_bio(
            &session->ssl, session,
            h2_bk_dtls_send, h2_bk_dtls_recv, NULL);
        mbedtls_ssl_set_timer_cb(
            &session->ssl, session,
            h2_bk_dtls_timer_set, h2_bk_dtls_timer_get);
        /* The pinned BK SDK exposes this callback without a config guard. */
        mbedtls_ssl_set_export_keys_cb(
            &session->ssl, h2_bk_dtls_export_keys, session);
    }
    if (result != 0) {
        h2_bk_dtls_destroy_impl(session);
        return identity_result != H2_PAL_OK ? identity_result : H2_PAL_ERR_IO;
    }
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_dtls_local_fingerprint(
    void *user,
    h2_pal_dtls_session_t *session,
    uint8_t out[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    (void)user;
    memcpy(out, session->local_fingerprint, sizeof(session->local_fingerprint));
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_dtls_remote_fingerprint(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    (void)user;
    if (session->handshake_complete) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    memcpy(session->remote_fingerprint, fingerprint,
           sizeof(session->remote_fingerprint));
    session->remote_fingerprint_set = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_dtls_verify_peer(
    h2_pal_dtls_session_t *session) {
    const mbedtls_x509_crt *certificate =
        mbedtls_ssl_get_peer_cert(&session->ssl);
    uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    if (certificate == NULL ||
        mbedtls_sha256(
            certificate->raw.p, certificate->raw.len,
            fingerprint, 0) != 0 ||
        memcmp(fingerprint, session->remote_fingerprint,
               sizeof(fingerprint)) != 0) {
        return H2_PAL_ERR_TLS_VERIFY;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_dtls_handshake(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len,
    uint64_t now_ms,
    uint64_t deadline_ms,
    int *out_complete) {
    (void)user;
    if (!session->remote_fingerprint_set) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->handshake_complete) {
        *out_complete = 1;
        return H2_PAL_OK;
    }
    if (now_ms >= deadline_ms) {
        return H2_PAL_ERR_TIMEOUT;
    }
    session->now_ms = now_ms;
    session->handshake_started = 1;
    h2_bk_dtls_set_input(session, datagram, datagram_len);
    int result = mbedtls_ssl_handshake(&session->ssl);
    h2_bk_dtls_clear_input(session);
    if (result == 0) {
        h2_pal_result_t verify_result = h2_bk_dtls_verify_peer(session);
        if (verify_result != H2_PAL_OK) {
            return verify_result;
        }
        session->handshake_complete = 1;
        *out_complete = 1;
        return H2_PAL_OK;
    }
    return result == MBEDTLS_ERR_SSL_WANT_READ ||
                   result == MBEDTLS_ERR_SSL_WANT_WRITE
               ? H2_PAL_ERR_WOULD_BLOCK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t h2_bk_dtls_next_deadline(
    void *user,
    h2_pal_dtls_session_t *session,
    uint64_t *out_deadline_ms) {
    uint64_t intermediate_deadline;
    (void)user;
    if (!session->handshake_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->handshake_complete || session->timer_final_ms == 0u) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    intermediate_deadline =
        session->timer_start_ms + session->timer_intermediate_ms;
    *out_deadline_ms = session->now_ms < intermediate_deadline
                           ? intermediate_deadline
                           : session->timer_start_ms + session->timer_final_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_dtls_flush(
    void *user, h2_pal_dtls_session_t *session) {
    (void)user;
    (void)session;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_dtls_profile(
    void *user,
    h2_pal_dtls_session_t *session,
    h2_pal_dtls_srtp_profile_t *out_profile) {
    (void)user;
    mbedtls_dtls_srtp_info info;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&session->ssl, &info);
    if (info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) !=
        MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_profile = H2_PAL_DTLS_SRTP_PROFILE_AES128_CM_SHA1_80;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_dtls_export(
    void *user,
    h2_pal_dtls_session_t *session,
    uint8_t *out,
    size_t out_len) {
    (void)user;
    if (!session->handshake_complete || !session->exporter_ready) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return mbedtls_ssl_tls_prf(
               session->prf,
               session->master_secret, session->master_secret_len,
               H2_BK_DTLS_SRTP_EXPORTER_LABEL,
               session->randoms, sizeof(session->randoms),
               out, out_len) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t h2_bk_dtls_write(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *data,
    size_t data_len) {
    (void)user;
    if (!session->handshake_complete || session->close_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (data_len > session->config.max_plaintext_size) {
        return H2_PAL_ERR_TRUNCATED;
    }
    int result = mbedtls_ssl_write(&session->ssl, data, data_len);
    if (result >= 0 && (size_t)result == data_len) {
        return H2_PAL_OK;
    }
    return result == MBEDTLS_ERR_SSL_WANT_READ ||
                   result == MBEDTLS_ERR_SSL_WANT_WRITE
               ? H2_PAL_ERR_WOULD_BLOCK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t h2_bk_dtls_consume_datagram(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len) {
    (void)user;
    if (!session->handshake_complete || session->close_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (datagram_len > session->config.max_datagram_size) {
        return H2_PAL_ERR_TRUNCATED;
    }
    h2_bk_dtls_set_input(session, datagram, datagram_len);
    for (;;) {
        int result = mbedtls_ssl_read(
            &session->ssl, session->plaintext,
            session->config.max_plaintext_size);
        if (result > 0) {
            h2_pal_result_t callback_result = session->config.plaintext(
                session->config.io_user, session->plaintext, (size_t)result);
            memset(session->plaintext, 0, (size_t)result);
            if (callback_result != H2_PAL_OK) {
                h2_bk_dtls_clear_input(session);
                return callback_result;
            }
            continue;
        }
        h2_bk_dtls_clear_input(session);
        if (result == 0) {
            return H2_PAL_ERR_CLOSED;
        }
        return result == MBEDTLS_ERR_SSL_WANT_READ ||
                       result == MBEDTLS_ERR_SSL_WANT_WRITE
                   ? H2_PAL_OK
                   : H2_PAL_ERR_IO;
    }
}

static h2_pal_result_t h2_bk_dtls_close(
    void *user, h2_pal_dtls_session_t *session) {
    (void)user;
    if (session->close_started) {
        return H2_PAL_OK;
    }
    session->close_started = 1;
    int result = mbedtls_ssl_close_notify(&session->ssl);
    return result == 0 || result == MBEDTLS_ERR_SSL_WANT_READ ||
                   result == MBEDTLS_ERR_SSL_WANT_WRITE
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static void h2_bk_dtls_destroy(
    void *user, h2_pal_dtls_session_t **session) {
    (void)user;
    h2_pal_dtls_session_t *owned = *session;
    *session = NULL;
    h2_bk_dtls_destroy_impl(owned);
}

static const h2_pal_dtls_vtable_t h2_bk_dtls_vtable = {
    .session_create = h2_bk_dtls_create,
    .session_get_local_fingerprint = h2_bk_dtls_local_fingerprint,
    .session_set_remote_fingerprint = h2_bk_dtls_remote_fingerprint,
    .session_handshake = h2_bk_dtls_handshake,
    .session_next_deadline_ms = h2_bk_dtls_next_deadline,
    .session_flush = h2_bk_dtls_flush,
    .session_get_srtp_profile = h2_bk_dtls_profile,
    .session_export_srtp_keying_material = h2_bk_dtls_export,
    .session_write = h2_bk_dtls_write,
    .session_consume_datagram = h2_bk_dtls_consume_datagram,
    .session_close = h2_bk_dtls_close,
    .session_destroy = h2_bk_dtls_destroy,
};

const h2_pal_dtls_api_t *h2_bk_platform_dtls_api(void) {
    static const h2_pal_dtls_api_t api = {
        .user = NULL,
        .vtable = &h2_bk_dtls_vtable,
    };
    return &api;
}

#else

const h2_pal_dtls_api_t *h2_bk_platform_dtls_api(void) {
    return h2_pal_unsupported_dtls_api();
}

#endif
