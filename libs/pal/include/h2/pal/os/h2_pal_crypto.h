#ifndef H2_PAL_CRYPTO_H
#define H2_PAL_CRYPTO_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_CRYPTO_X25519_KEY_SIZE 32u
#define H2_PAL_CRYPTO_AEAD_NONCE_SIZE 12u
#define H2_PAL_CRYPTO_AEAD_TAG_SIZE 16u
#define H2_PAL_CRYPTO_AES_BLOCK_SIZE 16u
#define H2_PAL_CRYPTO_MD5_SIZE 16u
#define H2_PAL_CRYPTO_SHA1_SIZE 20u
#define H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE 32u
#define H2_PAL_CRYPTO_P256_PUBLIC_KEY_SIZE 33u
#define H2_PAL_CRYPTO_P256_SIGNATURE_SIZE 64u

/** Authenticated-encryption algorithms exposed by Crypto PAL. */
typedef enum h2_pal_crypto_aead_algorithm {
    H2_PAL_CRYPTO_AEAD_AES_128_GCM = 1,
    H2_PAL_CRYPTO_AEAD_AES_256_GCM = 2,
    H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305 = 3,
} h2_pal_crypto_aead_algorithm_t;

/** RFC 7748 X25519 private scalar bytes in little-endian wire form. */
typedef struct h2_pal_x25519_private_key {
    uint8_t bytes[H2_PAL_CRYPTO_X25519_KEY_SIZE];
} h2_pal_x25519_private_key_t;

/** RFC 7748 X25519 public u-coordinate in little-endian wire form. */
typedef struct h2_pal_x25519_public_key {
    uint8_t bytes[H2_PAL_CRYPTO_X25519_KEY_SIZE];
} h2_pal_x25519_public_key_t;

/** RFC 7748 X25519 shared secret in little-endian wire form. */
typedef struct h2_pal_x25519_shared_secret {
    uint8_t bytes[H2_PAL_CRYPTO_X25519_KEY_SIZE];
} h2_pal_x25519_shared_secret_t;

/** X25519 private bytes and the matching public u-coordinate. */
typedef struct h2_pal_x25519_keypair {
    h2_pal_x25519_private_key_t private_key;
    h2_pal_x25519_public_key_t public_key;
} h2_pal_x25519_keypair_t;

/** Unsigned big-endian P-256 private scalar in the range [1, n - 1]. */
typedef struct h2_pal_p256_private_key {
    uint8_t bytes[H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE];
} h2_pal_p256_private_key_t;

/** Compressed SEC1 P-256 public key using prefix 0x02 or 0x03. */
typedef struct h2_pal_p256_public_key {
    uint8_t bytes[H2_PAL_CRYPTO_P256_PUBLIC_KEY_SIZE];
} h2_pal_p256_public_key_t;

/** P-256 private scalar and its matching compressed public key. */
typedef struct h2_pal_p256_keypair {
    h2_pal_p256_private_key_t private_key;
    h2_pal_p256_public_key_t public_key;
} h2_pal_p256_keypair_t;

/** Raw P-256 ECDSA signature encoded as fixed-width big-endian r || s. */
typedef struct h2_pal_p256_signature {
    uint8_t bytes[H2_PAL_CRYPTO_P256_SIGNATURE_SIZE];
} h2_pal_p256_signature_t;

/** Caller-owned variable-length output storage. */
typedef struct h2_pal_crypto_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} h2_pal_crypto_buf_t;

typedef struct h2_pal_crypto_vtable {
    h2_pal_result_t (*random)(void *user, uint8_t *out, size_t len);
    h2_pal_result_t (*x25519_keypair_generate)(
        void *user,
        h2_pal_x25519_keypair_t *out_keypair);
    h2_pal_result_t (*x25519_public_key_from_private)(
        void *user,
        const h2_pal_x25519_private_key_t *private_key,
        h2_pal_x25519_public_key_t *out_public_key);
    h2_pal_result_t (*x25519_shared_secret)(
        void *user,
        const h2_pal_x25519_private_key_t *local_private_key,
        const h2_pal_x25519_public_key_t *remote_public_key,
        h2_pal_x25519_shared_secret_t *out_shared_secret);
    h2_pal_result_t (*hkdf_sha256)(
        void *user,
        const uint8_t *secret,
        size_t secret_len,
        const uint8_t *salt,
        size_t salt_len,
        const uint8_t *info,
        size_t info_len,
        uint8_t *out,
        size_t out_len);
    h2_pal_result_t (*aead_seal)(
        void *user,
        h2_pal_crypto_aead_algorithm_t algorithm,
        const uint8_t *key,
        size_t key_len,
        const uint8_t *nonce,
        size_t nonce_len,
        const uint8_t *plaintext,
        size_t plaintext_len,
        const uint8_t *aad,
        size_t aad_len,
        h2_pal_crypto_buf_t *out_ciphertext);
    h2_pal_result_t (*aead_open)(
        void *user,
        h2_pal_crypto_aead_algorithm_t algorithm,
        const uint8_t *key,
        size_t key_len,
        const uint8_t *nonce,
        size_t nonce_len,
        const uint8_t *ciphertext,
        size_t ciphertext_len,
        const uint8_t *aad,
        size_t aad_len,
        h2_pal_crypto_buf_t *out_plaintext);
    h2_pal_result_t (*aes_ctr_xor)(
        void *user,
        const uint8_t *key,
        size_t key_len,
        const uint8_t initial_counter[H2_PAL_CRYPTO_AES_BLOCK_SIZE],
        const uint8_t *input,
        size_t input_len,
        h2_pal_crypto_buf_t *out);
    h2_pal_result_t (*md5)(
        void *user,
        const uint8_t *input,
        size_t input_len,
        uint8_t out_digest[H2_PAL_CRYPTO_MD5_SIZE]);
    h2_pal_result_t (*hmac_sha1)(
        void *user,
        const uint8_t *key,
        size_t key_len,
        const uint8_t *input,
        size_t input_len,
        uint8_t out_digest[H2_PAL_CRYPTO_SHA1_SIZE]);
    h2_pal_result_t (*p256_keypair_from_private)(
        void *user,
        const h2_pal_p256_private_key_t *private_key,
        h2_pal_p256_keypair_t *out_keypair);
    h2_pal_result_t (*p256_keypair_generate)(
        void *user,
        h2_pal_p256_keypair_t *out_keypair);
    h2_pal_result_t (*p256_public_key_validate)(
        void *user,
        const h2_pal_p256_public_key_t *public_key);
    h2_pal_result_t (*ecdsa_p256_sha256_sign)(
        void *user,
        const h2_pal_p256_private_key_t *private_key,
        const uint8_t *message,
        size_t message_len,
        h2_pal_p256_signature_t *out_signature);
    h2_pal_result_t (*ecdsa_p256_sha256_verify)(
        void *user,
        const h2_pal_p256_public_key_t *public_key,
        const uint8_t *message,
        size_t message_len,
        const h2_pal_p256_signature_t *signature);
} h2_pal_crypto_vtable_t;

typedef struct h2_pal_crypto_api {
    void *user;
    const h2_pal_crypto_vtable_t *vtable;
} h2_pal_crypto_api_t;

static inline h2_pal_result_t h2_pal_crypto_random(
    const h2_pal_crypto_api_t *api,
    uint8_t *out,
    size_t len) {
    if (out == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->random == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->random(api->user, out, len);
}

static inline h2_pal_result_t h2_pal_crypto_x25519_keypair_generate(
    const h2_pal_crypto_api_t *api,
    h2_pal_x25519_keypair_t *out_keypair) {
    if (out_keypair == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->x25519_keypair_generate == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->x25519_keypair_generate(api->user, out_keypair);
}

static inline h2_pal_result_t h2_pal_crypto_x25519_public_key_from_private(
    const h2_pal_crypto_api_t *api,
    const h2_pal_x25519_private_key_t *private_key,
    h2_pal_x25519_public_key_t *out_public_key) {
    if (private_key == NULL || out_public_key == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->x25519_public_key_from_private == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->x25519_public_key_from_private(
        api->user, private_key, out_public_key);
}

static inline h2_pal_result_t h2_pal_crypto_x25519_shared_secret(
    const h2_pal_crypto_api_t *api,
    const h2_pal_x25519_private_key_t *local_private_key,
    const h2_pal_x25519_public_key_t *remote_public_key,
    h2_pal_x25519_shared_secret_t *out_shared_secret) {
    if (local_private_key == NULL || remote_public_key == NULL ||
        out_shared_secret == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->x25519_shared_secret == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->x25519_shared_secret(
        api->user, local_private_key, remote_public_key, out_shared_secret);
}

static inline h2_pal_result_t h2_pal_crypto_hkdf_sha256(
    const h2_pal_crypto_api_t *api,
    const uint8_t *secret,
    size_t secret_len,
    const uint8_t *salt,
    size_t salt_len,
    const uint8_t *info,
    size_t info_len,
    uint8_t *out,
    size_t out_len) {
    if ((secret == NULL && secret_len != 0u) ||
        (salt == NULL && salt_len != 0u) ||
        (info == NULL && info_len != 0u) ||
        (out == NULL && out_len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->hkdf_sha256 == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->hkdf_sha256(
        api->user, secret, secret_len, salt, salt_len,
        info, info_len, out, out_len);
}

static inline h2_pal_result_t h2_pal_crypto_aead_seal(
    const h2_pal_crypto_api_t *api,
    h2_pal_crypto_aead_algorithm_t algorithm,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *nonce,
    size_t nonce_len,
    const uint8_t *plaintext,
    size_t plaintext_len,
    const uint8_t *aad,
    size_t aad_len,
    h2_pal_crypto_buf_t *out_ciphertext) {
    if ((key == NULL && key_len != 0u) ||
        (nonce == NULL && nonce_len != 0u) ||
        (plaintext == NULL && plaintext_len != 0u) ||
        (aad == NULL && aad_len != 0u) || out_ciphertext == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_ciphertext->len = 0u;
    if (api == NULL || api->vtable == NULL ||
        api->vtable->aead_seal == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->aead_seal(
        api->user, algorithm, key, key_len, nonce, nonce_len,
        plaintext, plaintext_len, aad, aad_len, out_ciphertext);
}

static inline h2_pal_result_t h2_pal_crypto_aead_open(
    const h2_pal_crypto_api_t *api,
    h2_pal_crypto_aead_algorithm_t algorithm,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *nonce,
    size_t nonce_len,
    const uint8_t *ciphertext,
    size_t ciphertext_len,
    const uint8_t *aad,
    size_t aad_len,
    h2_pal_crypto_buf_t *out_plaintext) {
    if ((key == NULL && key_len != 0u) ||
        (nonce == NULL && nonce_len != 0u) ||
        (ciphertext == NULL && ciphertext_len != 0u) ||
        (aad == NULL && aad_len != 0u) || out_plaintext == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_plaintext->len = 0u;
    if (api == NULL || api->vtable == NULL ||
        api->vtable->aead_open == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->aead_open(
        api->user, algorithm, key, key_len, nonce, nonce_len,
        ciphertext, ciphertext_len, aad, aad_len, out_plaintext);
}

static inline h2_pal_result_t h2_pal_crypto_aes_ctr_xor(
    const h2_pal_crypto_api_t *api,
    const uint8_t *key,
    size_t key_len,
    const uint8_t initial_counter[H2_PAL_CRYPTO_AES_BLOCK_SIZE],
    const uint8_t *input,
    size_t input_len,
    h2_pal_crypto_buf_t *out) {
    if (key == NULL || initial_counter == NULL ||
        (input == NULL && input_len != 0u) || out == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out->len = 0u;
    if (api == NULL || api->vtable == NULL ||
        api->vtable->aes_ctr_xor == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->aes_ctr_xor(
        api->user, key, key_len, initial_counter, input, input_len, out);
}

static inline h2_pal_result_t h2_pal_crypto_md5(
    const h2_pal_crypto_api_t *api,
    const uint8_t *input,
    size_t input_len,
    uint8_t out_digest[H2_PAL_CRYPTO_MD5_SIZE]) {
    if ((input == NULL && input_len != 0u) || out_digest == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->md5 == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->md5(api->user, input, input_len, out_digest);
}

static inline h2_pal_result_t h2_pal_crypto_hmac_sha1(
    const h2_pal_crypto_api_t *api,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *input,
    size_t input_len,
    uint8_t out_digest[H2_PAL_CRYPTO_SHA1_SIZE]) {
    if ((key == NULL && key_len != 0u) ||
        (input == NULL && input_len != 0u) || out_digest == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->hmac_sha1 == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->hmac_sha1(
        api->user, key, key_len, input, input_len, out_digest);
}

static inline h2_pal_result_t h2_pal_crypto_p256_keypair_from_private(
    const h2_pal_crypto_api_t *api,
    const h2_pal_p256_private_key_t *private_key,
    h2_pal_p256_keypair_t *out_keypair) {
    if (private_key == NULL || out_keypair == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->p256_keypair_from_private == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->p256_keypair_from_private(
        api->user, private_key, out_keypair);
}

static inline h2_pal_result_t h2_pal_crypto_p256_keypair_generate(
    const h2_pal_crypto_api_t *api,
    h2_pal_p256_keypair_t *out_keypair) {
    if (out_keypair == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->p256_keypair_generate == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->p256_keypair_generate(api->user, out_keypair);
}

static inline h2_pal_result_t h2_pal_crypto_p256_public_key_validate(
    const h2_pal_crypto_api_t *api,
    const h2_pal_p256_public_key_t *public_key) {
    if (public_key == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->p256_public_key_validate == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->p256_public_key_validate(api->user, public_key);
}

static inline h2_pal_result_t h2_pal_crypto_ecdsa_p256_sha256_sign(
    const h2_pal_crypto_api_t *api,
    const h2_pal_p256_private_key_t *private_key,
    const uint8_t *message,
    size_t message_len,
    h2_pal_p256_signature_t *out_signature) {
    if (private_key == NULL || (message == NULL && message_len != 0u) ||
        out_signature == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->ecdsa_p256_sha256_sign == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->ecdsa_p256_sha256_sign(
        api->user, private_key, message, message_len, out_signature);
}

static inline h2_pal_result_t h2_pal_crypto_ecdsa_p256_sha256_verify(
    const h2_pal_crypto_api_t *api,
    const h2_pal_p256_public_key_t *public_key,
    const uint8_t *message,
    size_t message_len,
    const h2_pal_p256_signature_t *signature) {
    if (public_key == NULL || (message == NULL && message_len != 0u) ||
        signature == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->ecdsa_p256_sha256_verify == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->ecdsa_p256_sha256_verify(
        api->user, public_key, message, message_len, signature);
}

#ifdef __cplusplus
}
#endif

#endif
