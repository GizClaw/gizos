#ifndef H2_PAL_DTLS_H
#define H2_PAL_DTLS_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE 32u
#define H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE 60u

typedef struct h2_pal_dtls_session h2_pal_dtls_session_t;

typedef enum h2_pal_dtls_role {
    H2_PAL_DTLS_ROLE_CLIENT = 0,
    H2_PAL_DTLS_ROLE_SERVER = 1,
} h2_pal_dtls_role_t;

typedef enum h2_pal_dtls_srtp_profile {
    H2_PAL_DTLS_SRTP_PROFILE_AES128_CM_SHA1_80 = 1,
} h2_pal_dtls_srtp_profile_t;

/** Sends one complete provider-owned DTLS datagram synchronously. */
typedef h2_pal_result_t (*h2_pal_dtls_send_fn)(
    void *user,
    const uint8_t *datagram,
    size_t datagram_len);

/** Consumes one complete borrowed plaintext application record. */
typedef h2_pal_result_t (*h2_pal_dtls_plaintext_fn)(
    void *user,
    const uint8_t *plaintext,
    size_t plaintext_len);

typedef struct h2_pal_dtls_session_config {
    h2_pal_dtls_role_t role;
    size_t max_datagram_size;
    size_t max_plaintext_size;
    size_t max_pending_output_bytes;
    h2_pal_dtls_send_fn send;
    h2_pal_dtls_plaintext_fn plaintext;
    void *io_user;
} h2_pal_dtls_session_config_t;

typedef struct h2_pal_dtls_vtable {
    h2_pal_result_t (*session_create)(
        void *user,
        const h2_pal_dtls_session_config_t *config,
        h2_pal_dtls_session_t **out_session);
    h2_pal_result_t (*session_get_local_fingerprint)(
        void *user,
        h2_pal_dtls_session_t *session,
        uint8_t out_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]);
    h2_pal_result_t (*session_set_remote_fingerprint)(
        void *user,
        h2_pal_dtls_session_t *session,
        const uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]);
    h2_pal_result_t (*session_handshake)(
        void *user,
        h2_pal_dtls_session_t *session,
        const uint8_t *datagram,
        size_t datagram_len,
        uint64_t now_ms,
        uint64_t deadline_ms,
        int *out_complete);
    h2_pal_result_t (*session_next_deadline_ms)(
        void *user,
        h2_pal_dtls_session_t *session,
        uint64_t *out_deadline_ms);
    h2_pal_result_t (*session_flush)(
        void *user,
        h2_pal_dtls_session_t *session);
    h2_pal_result_t (*session_get_srtp_profile)(
        void *user,
        h2_pal_dtls_session_t *session,
        h2_pal_dtls_srtp_profile_t *out_profile);
    h2_pal_result_t (*session_export_srtp_keying_material)(
        void *user,
        h2_pal_dtls_session_t *session,
        uint8_t *out,
        size_t out_len);
    h2_pal_result_t (*session_write)(
        void *user,
        h2_pal_dtls_session_t *session,
        const uint8_t *plaintext,
        size_t plaintext_len);
    h2_pal_result_t (*session_consume_datagram)(
        void *user,
        h2_pal_dtls_session_t *session,
        const uint8_t *datagram,
        size_t datagram_len);
    h2_pal_result_t (*session_close)(
        void *user,
        h2_pal_dtls_session_t *session);
    void (*session_destroy)(
        void *user,
        h2_pal_dtls_session_t **session);
} h2_pal_dtls_vtable_t;

typedef struct h2_pal_dtls_api {
    void *user;
    const h2_pal_dtls_vtable_t *vtable;
} h2_pal_dtls_api_t;

static inline h2_pal_result_t h2_pal_dtls_session_create(
    const h2_pal_dtls_api_t *api,
    const h2_pal_dtls_session_config_t *config,
    h2_pal_dtls_session_t **out_session) {
    if (out_session != NULL) {
        *out_session = NULL;
    }
    if (config == NULL || out_session == NULL || config->send == NULL ||
        config->plaintext == NULL || config->max_datagram_size == 0u ||
        config->max_plaintext_size == 0u ||
        config->max_pending_output_bytes == 0u ||
        (config->role != H2_PAL_DTLS_ROLE_CLIENT &&
         config->role != H2_PAL_DTLS_ROLE_SERVER)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_create == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_create(api->user, config, out_session);
}

static inline h2_pal_result_t h2_pal_dtls_session_get_local_fingerprint(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session,
    uint8_t out_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    if (session == NULL || out_fingerprint == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_get_local_fingerprint == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_get_local_fingerprint(
        api->user, session, out_fingerprint);
}

static inline h2_pal_result_t h2_pal_dtls_session_set_remote_fingerprint(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session,
    const uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    if (session == NULL || fingerprint == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_set_remote_fingerprint == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_set_remote_fingerprint(
        api->user, session, fingerprint);
}

static inline h2_pal_result_t h2_pal_dtls_session_handshake(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len,
    uint64_t now_ms,
    uint64_t deadline_ms,
    int *out_complete) {
    if (out_complete != NULL) {
        *out_complete = 0;
    }
    if (session == NULL || out_complete == NULL || now_ms > deadline_ms ||
        (datagram == NULL && datagram_len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_handshake == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_handshake(
        api->user, session, datagram, datagram_len,
        now_ms, deadline_ms, out_complete);
}

static inline h2_pal_result_t h2_pal_dtls_session_next_deadline_ms(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session,
    uint64_t *out_deadline_ms) {
    if (out_deadline_ms != NULL) {
        *out_deadline_ms = 0u;
    }
    if (session == NULL || out_deadline_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_next_deadline_ms == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_next_deadline_ms(
        api->user, session, out_deadline_ms);
}

static inline h2_pal_result_t h2_pal_dtls_session_flush(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session) {
    if (session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_flush == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_flush(api->user, session);
}

static inline h2_pal_result_t h2_pal_dtls_session_get_srtp_profile(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session,
    h2_pal_dtls_srtp_profile_t *out_profile) {
    if (out_profile != NULL) {
        *out_profile = (h2_pal_dtls_srtp_profile_t)0;
    }
    if (session == NULL || out_profile == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_get_srtp_profile == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_get_srtp_profile(
        api->user, session, out_profile);
}

static inline h2_pal_result_t h2_pal_dtls_session_export_srtp_keying_material(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session,
    uint8_t *out,
    size_t out_len) {
    if (out != NULL &&
        out_len == H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE) {
        memset(out, 0, out_len);
    }
    if (session == NULL || out == NULL ||
        out_len != H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_export_srtp_keying_material == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_export_srtp_keying_material(
        api->user, session, out, out_len);
}

static inline h2_pal_result_t h2_pal_dtls_session_write(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session,
    const uint8_t *plaintext,
    size_t plaintext_len) {
    if (session == NULL || (plaintext == NULL && plaintext_len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_write == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_write(
        api->user, session, plaintext, plaintext_len);
}

static inline h2_pal_result_t h2_pal_dtls_session_consume_datagram(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len) {
    if (session == NULL || datagram == NULL || datagram_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_consume_datagram == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_consume_datagram(
        api->user, session, datagram, datagram_len);
}

static inline h2_pal_result_t h2_pal_dtls_session_close(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t *session) {
    if (session == NULL) {
        return H2_PAL_OK;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_close == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_close(api->user, session);
}

static inline void h2_pal_dtls_session_destroy(
    const h2_pal_dtls_api_t *api,
    h2_pal_dtls_session_t **session) {
    if (session == NULL || *session == NULL || api == NULL ||
        api->vtable == NULL || api->vtable->session_destroy == NULL) {
        return;
    }
    api->vtable->session_destroy(api->user, session);
}

#ifdef __cplusplus
}
#endif

#endif
