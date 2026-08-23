#ifndef H2_LIBSRTP_CRYPTO_INTERNAL_H
#define H2_LIBSRTP_CRYPTO_INTERNAL_H

#include "h2_libsrtp_internal.h"

#include "auth.h"
#include "cipher.h"

typedef struct h2_libsrtp_icm_state {
    uint8_t key[32];
    uint8_t offset[16];
    uint8_t counter[16];
    uint8_t keystream[16];
    size_t key_len;
    size_t keystream_offset;
} h2_libsrtp_icm_state_t;

typedef struct h2_libsrtp_gcm_state {
    uint8_t key[32];
    uint8_t iv[H2_PAL_CRYPTO_AEAD_NONCE_SIZE];
    uint8_t tag[H2_PAL_CRYPTO_AEAD_TAG_SIZE];
    size_t key_len;
    size_t tag_len;
    size_t aad_len;
    srtp_cipher_direction_t direction;
    uint8_t aad[];
} h2_libsrtp_gcm_state_t;

typedef struct h2_libsrtp_hmac_state {
    uint8_t key[H2_PAL_CRYPTO_SHA1_SIZE];
    size_t key_len;
    size_t data_len;
    size_t capacity;
    uint8_t data[];
} h2_libsrtp_hmac_state_t;

#endif
