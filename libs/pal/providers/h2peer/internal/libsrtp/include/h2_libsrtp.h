#ifndef H2_LIBSRTP_H
#define H2_LIBSRTP_H

#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LIBSRTP_MASTER_KEY_SIZE 16u
#define H2_LIBSRTP_AES_CM_SALT_SIZE 14u
#define H2_LIBSRTP_AEAD_SALT_SIZE 12u

typedef struct h2_libsrtp_session h2_libsrtp_session_t;

typedef enum h2_libsrtp_profile {
    H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80 = 1,
    H2_LIBSRTP_PROFILE_AEAD_AES_128_GCM = 2,
} h2_libsrtp_profile_t;

typedef enum h2_libsrtp_direction {
    H2_LIBSRTP_DIRECTION_INBOUND = 0,
    H2_LIBSRTP_DIRECTION_OUTBOUND = 1,
} h2_libsrtp_direction_t;

typedef enum h2_libsrtp_ssrc_policy {
    H2_LIBSRTP_SSRC_SPECIFIC = 0,
    H2_LIBSRTP_SSRC_ANY = 1,
} h2_libsrtp_ssrc_policy_t;

typedef struct h2_libsrtp_config {
    /** Memory provider copied by value and used for all owned allocations. */
    h2_pal_mem_api_t mem;
    /** Crypto provider copied by value and used by every SRTP primitive. */
    h2_pal_crypto_api_t crypto;
    /** Maximum plaintext or protected packet size accepted by a session. */
    size_t max_packet_size;
} h2_libsrtp_config_t;

typedef struct h2_libsrtp_session_config {
    h2_libsrtp_profile_t profile;
    h2_libsrtp_direction_t direction;
    h2_libsrtp_ssrc_policy_t ssrc_policy;
    uint32_t ssrc;
    const uint8_t *master_key;
    size_t master_key_len;
    const uint8_t *master_salt;
    size_t master_salt_len;
} h2_libsrtp_session_config_t;

/**
 * Initializes the process-global libSRTP kernel.
 *
 * Repeated calls with the same Memory and Crypto provider add owner
 * references. A different provider is rejected until all owners and sessions
 * have been released.
 */
h2_pal_result_t h2_libsrtp_init(const h2_libsrtp_config_t *config);

/** Releases one owner reference; final release requires no live sessions. */
h2_pal_result_t h2_libsrtp_deinit(void);

/**
 * Creates one bounded SRTP session.
 *
 * The session copies the key material and allocates all reusable packet
 * scratch space before returning. An ANY-SSRC session also reserves bounded
 * storage for stream contexts, so packet operations never call the Memory PAL.
 * The caller retains ownership of config.
 */
h2_pal_result_t h2_libsrtp_session_create(
    const h2_libsrtp_session_config_t *config,
    h2_libsrtp_session_t **out_session);

/** Destroys a session, clears its key material, and sets the pointer to NULL. */
void h2_libsrtp_session_destroy(h2_libsrtp_session_t **session);

/** Protects one RTP packet in place and commits packet and length on success. */
h2_pal_result_t h2_libsrtp_protect_rtp(
    h2_libsrtp_session_t *session,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len);

/** Authenticates and decrypts one RTP packet in place. */
h2_pal_result_t h2_libsrtp_unprotect_rtp(
    h2_libsrtp_session_t *session,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len);

/** Protects one RTCP packet in place and commits packet and length on success. */
h2_pal_result_t h2_libsrtp_protect_rtcp(
    h2_libsrtp_session_t *session,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len);

/** Authenticates and decrypts one RTCP packet in place. */
h2_pal_result_t h2_libsrtp_unprotect_rtcp(
    h2_libsrtp_session_t *session,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len);

#ifdef __cplusplus
}
#endif

#endif
