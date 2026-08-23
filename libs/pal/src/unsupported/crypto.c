#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_crypto_random(void *p0, uint8_t *p1, size_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_x25519_keypair_generate(
    void *p0, h2_pal_x25519_keypair_t *p1) {
    (void)p0;
    if (p1 != NULL) {
        *p1 = (h2_pal_x25519_keypair_t){0};
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_x25519_public_key_from_private(
    void *p0,
    const h2_pal_x25519_private_key_t *p1,
    h2_pal_x25519_public_key_t *p2) {
    (void)p0;
    (void)p1;
    if (p2 != NULL) {
        *p2 = (h2_pal_x25519_public_key_t){0};
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_x25519_shared_secret(
    void *p0,
    const h2_pal_x25519_private_key_t *p1,
    const h2_pal_x25519_public_key_t *p2,
    h2_pal_x25519_shared_secret_t *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    if (p3 != NULL) {
        *p3 = (h2_pal_x25519_shared_secret_t){0};
    }
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_hkdf_sha256(
    void *p0, const uint8_t *p1, size_t p2, const uint8_t *p3, size_t p4,
    const uint8_t *p5, size_t p6, uint8_t *p7, size_t p8) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    (void)p7;
    (void)p8;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_aead_seal(void *p0, h2_pal_crypto_aead_algorithm_t p1, const uint8_t *p2, size_t p3, const uint8_t *p4, size_t p5, const uint8_t *p6, size_t p7, const uint8_t *p8, size_t p9, h2_pal_crypto_buf_t *p10) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    (void)p7;
    (void)p8;
    (void)p9;
    (void)p10;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_aead_open(void *p0, h2_pal_crypto_aead_algorithm_t p1, const uint8_t *p2, size_t p3, const uint8_t *p4, size_t p5, const uint8_t *p6, size_t p7, const uint8_t *p8, size_t p9, h2_pal_crypto_buf_t *p10) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    (void)p7;
    (void)p8;
    (void)p9;
    (void)p10;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_aes_ctr_xor(
    void *p0, const uint8_t *p1, size_t p2, const uint8_t p3[16],
    const uint8_t *p4, size_t p5, h2_pal_crypto_buf_t *p6) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_md5(
    void *p0, const uint8_t *p1, size_t p2, uint8_t p3[16]) {
    (void)p0;
    (void)p1;
    (void)p2;
    if (p3 != NULL) {
        memset(p3, 0, H2_PAL_CRYPTO_MD5_SIZE);
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_hmac_sha1(
    void *p0, const uint8_t *p1, size_t p2, const uint8_t *p3, size_t p4,
    uint8_t p5[20]) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    if (p5 != NULL) {
        memset(p5, 0, H2_PAL_CRYPTO_SHA1_SIZE);
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_p256_keypair_from_private(void *p0, const h2_pal_p256_private_key_t *p1, h2_pal_p256_keypair_t *p2) {
    (void)p0;
    (void)p1;
    if (p2 != NULL) {
        *p2 = (h2_pal_p256_keypair_t){0};
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_p256_keypair_generate(void *p0, h2_pal_p256_keypair_t *p1) {
    (void)p0;
    if (p1 != NULL) {
        *p1 = (h2_pal_p256_keypair_t){0};
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_p256_public_key_validate(void *p0, const h2_pal_p256_public_key_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_ecdsa_p256_sha256_sign(void *p0, const h2_pal_p256_private_key_t *p1, const uint8_t *p2, size_t p3, h2_pal_p256_signature_t *p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    if (p4 != NULL) {
        *p4 = (h2_pal_p256_signature_t){0};
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_crypto_ecdsa_p256_sha256_verify(void *p0, const h2_pal_p256_public_key_t *p1, const uint8_t *p2, size_t p3, const h2_pal_p256_signature_t *p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_crypto_vtable_t unsupported_crypto_vtable = {
    .random = unsupported_crypto_random,
    .x25519_keypair_generate = unsupported_crypto_x25519_keypair_generate,
    .x25519_public_key_from_private = unsupported_crypto_x25519_public_key_from_private,
    .x25519_shared_secret = unsupported_crypto_x25519_shared_secret,
    .hkdf_sha256 = unsupported_crypto_hkdf_sha256,
    .aead_seal = unsupported_crypto_aead_seal,
    .aead_open = unsupported_crypto_aead_open,
    .aes_ctr_xor = unsupported_crypto_aes_ctr_xor,
    .md5 = unsupported_crypto_md5,
    .hmac_sha1 = unsupported_crypto_hmac_sha1,
    .p256_keypair_from_private = unsupported_crypto_p256_keypair_from_private,
    .p256_keypair_generate = unsupported_crypto_p256_keypair_generate,
    .p256_public_key_validate = unsupported_crypto_p256_public_key_validate,
    .ecdsa_p256_sha256_sign = unsupported_crypto_ecdsa_p256_sha256_sign,
    .ecdsa_p256_sha256_verify = unsupported_crypto_ecdsa_p256_sha256_verify,
};
static const h2_pal_crypto_api_t unsupported_crypto_api = { .user = NULL, .vtable = &unsupported_crypto_vtable };
const h2_pal_crypto_api_t *h2_pal_unsupported_crypto_api(void) { return &unsupported_crypto_api; }
