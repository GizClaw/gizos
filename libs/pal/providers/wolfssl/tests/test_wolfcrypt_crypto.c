#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int test_entropy(void *user, uint8_t *out, size_t len) {
    uint32_t *state = (uint32_t *)user;
    size_t index;
    for (index = 0u; index < len; ++index) {
        *state = *state * 1664525u + 1013904223u;
        out[index] = (uint8_t)(*state >> 24);
    }
    return H2_PAL_OK;
}

static void test_direct_vtable_null_arguments(
    const h2_pal_crypto_api_t *api) {
    uint8_t byte = 0u;
    h2_pal_x25519_private_key_t key = {0};
    h2_pal_x25519_public_key_t public_key = {0};
    h2_pal_x25519_shared_secret_t shared_secret = {0};
    h2_pal_p256_private_key_t p256_private = {0};
    h2_pal_p256_keypair_t p256_keypair = {0};
    h2_pal_p256_public_key_t p256_public = {0};
    h2_pal_p256_signature_t signature = {0};
    h2_pal_crypto_buf_t output = {.data = &byte, .cap = 1u};

    assert(api->vtable->x25519_public_key_from_private(
               api->user, NULL, &public_key) == H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->x25519_public_key_from_private(
               api->user, &key, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->x25519_keypair_generate(api->user, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->x25519_shared_secret(
               api->user, NULL, &public_key, &shared_secret) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->hkdf_sha256(
               api->user, NULL, 1u, NULL, 0u,
               NULL, 0u, &byte, 1u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->aead_seal(
               api->user, H2_PAL_CRYPTO_AEAD_AES_128_GCM,
               NULL, 0u, &byte, 12u,
               NULL, 0u, NULL, 0u, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->aead_open(
               api->user, H2_PAL_CRYPTO_AEAD_AES_128_GCM,
               NULL, 0u, &byte, 12u,
               NULL, 1u, NULL, 0u, &output) == H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->p256_keypair_from_private(
               api->user, NULL, &p256_keypair) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->p256_keypair_generate(api->user, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->p256_public_key_validate(api->user, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->ecdsa_p256_sha256_sign(
               api->user, &p256_private, NULL, 1u, &signature) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->ecdsa_p256_sha256_verify(
               api->user, &p256_public, NULL, 0u, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
}

static void test_x25519(const h2_pal_crypto_api_t *api) {
    static const uint8_t private_key[32] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
    };
    static const uint8_t public_key[32] = {
        0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
        0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
        0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
        0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
    };
    static const uint8_t remote_key[32] = {
        0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4,
        0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
        0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
        0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f,
    };
    static const uint8_t shared_key[32] = {
        0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1,
        0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
        0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33,
        0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42,
    };
    h2_pal_x25519_private_key_t private_value;
    h2_pal_x25519_public_key_t derived_public;
    h2_pal_x25519_public_key_t remote;
    h2_pal_x25519_shared_secret_t shared;

    memcpy(private_value.bytes, private_key, sizeof(private_key));
    memcpy(remote.bytes, remote_key, sizeof(remote_key));
    assert(h2_pal_crypto_x25519_public_key_from_private(
        api, &private_value, &derived_public) == H2_PAL_OK);
    assert(memcmp(derived_public.bytes, public_key, sizeof(public_key)) == 0);
    assert(h2_pal_crypto_x25519_shared_secret(
        api, &private_value, &remote, &shared) == H2_PAL_OK);
    assert(memcmp(shared.bytes, shared_key, sizeof(shared_key)) == 0);

    memset(remote.bytes, 0, sizeof(remote.bytes));
    assert(h2_pal_crypto_x25519_shared_secret(
               api, &private_value, &remote, &shared) ==
           H2_PAL_ERR_FORMAT);
    memcpy(remote.bytes, remote_key, sizeof(remote_key));
    remote.bytes[31] |= 0x80u;
    assert(h2_pal_crypto_x25519_shared_secret(
               api, &private_value, &remote, &shared) == H2_PAL_OK);
    assert(memcmp(shared.bytes, shared_key, sizeof(shared_key)) == 0);

    h2_pal_x25519_keypair_t generated;
    assert(h2_pal_crypto_x25519_keypair_generate(api, &generated) ==
           H2_PAL_OK);
    assert(h2_pal_crypto_x25519_public_key_from_private(
               api, &generated.private_key, &derived_public) == H2_PAL_OK);
    assert(memcmp(
               generated.public_key.bytes, derived_public.bytes,
               sizeof(derived_public.bytes)) == 0);
}

static void test_hkdf(const h2_pal_crypto_api_t *api) {
    static const uint8_t ikm[22] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    };
    static const uint8_t salt[13] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    };
    static const uint8_t info_bytes[10] = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4,
        0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    };
    static const uint8_t expected[42] = {
        0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a,
        0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36, 0x2f, 0x2a,
        0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c,
        0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf,
        0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18,
        0x58, 0x65,
    };
    uint8_t output[42];

    assert(h2_pal_crypto_hkdf_sha256(
        api, ikm, sizeof(ikm), salt, sizeof(salt),
        info_bytes, sizeof(info_bytes),
        output, sizeof(output)) == H2_PAL_OK);
    assert(memcmp(output, expected, sizeof(expected)) == 0);
    assert(h2_pal_crypto_hkdf_sha256(
        api, NULL, 0u, NULL, 0u, NULL, 0u,
        NULL, 0u) == H2_PAL_OK);
}

static void test_aes_empty_kat(
    const h2_pal_crypto_api_t *api,
    h2_pal_crypto_aead_algorithm_t mode,
    size_t key_len,
    const uint8_t expected_tag[16]) {
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t output[16];
    h2_pal_crypto_buf_t sealed = {
        .data = output,
        .cap = sizeof(output),
    };

    assert(h2_pal_crypto_aead_seal(
        api, mode, key, key_len, nonce, sizeof(nonce),
        NULL, 0u, NULL, 0u, &sealed) == H2_PAL_OK);
    assert(sealed.len == sizeof(output));
    assert(memcmp(output, expected_tag, sizeof(output)) == 0);
    h2_pal_crypto_buf_t opened = {0};
    assert(h2_pal_crypto_aead_open(
        api, mode, key, key_len, nonce, sizeof(nonce),
        output, sizeof(output), NULL, 0u, &opened) == H2_PAL_OK);
    assert(opened.len == 0u);
}

static void test_chacha20_poly1305_kat(
    const h2_pal_crypto_api_t *api) {
    static const uint8_t key[32] = {
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    };
    static const uint8_t nonce[12] = {
        0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
        0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    };
    static const uint8_t aad[12] = {
        0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
        0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    };
    static const uint8_t plaintext[] =
        "Ladies and Gentlemen of the class of '99: If I could offer you only "
        "one tip for the future, sunscreen would be it.";
    static const uint8_t expected[sizeof(plaintext) - 1u + 16u] = {
        0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb,
        0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
        0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
        0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6,
        0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67, 0x12,
        0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
        0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29,
        0x05, 0xd6, 0xa5, 0xb6, 0x7e, 0xcd, 0x3b, 0x36,
        0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
        0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58,
        0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94,
        0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
        0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d,
        0xe5, 0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b,
        0x61, 0x16, 0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09,
        0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60,
        0x06, 0x91,
    };
    uint8_t output[sizeof(expected)];
    h2_pal_crypto_buf_t sealed = {
        .data = output,
        .cap = sizeof(output),
    };

    assert(h2_pal_crypto_aead_seal(
        api, H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305,
        key, sizeof(key), nonce, sizeof(nonce),
        plaintext, sizeof(plaintext) - 1u, aad, sizeof(aad),
        &sealed) == H2_PAL_OK);
    assert(sealed.len == sizeof(expected));
    assert(memcmp(output, expected, sizeof(expected)) == 0);
}

static void test_aead(
    const h2_pal_crypto_api_t *api,
    h2_pal_crypto_aead_algorithm_t mode,
    size_t key_len) {
    const uint8_t plaintext[] = "wolfCrypt PAL";
    const uint8_t aad[] = {1u, 2u, 3u};
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};
    uint8_t ciphertext[sizeof(plaintext) + 16u];
    uint8_t opened[sizeof(plaintext)];
    h2_pal_crypto_buf_t sealed = {
        .data = ciphertext,
        .len = 123u,
        .cap = sizeof(ciphertext),
    };
    h2_pal_crypto_buf_t plain = {
        .data = opened,
        .len = 123u,
        .cap = sizeof(opened),
    };

    assert(h2_pal_crypto_aead_seal(
        api, mode, key, key_len, nonce, sizeof(nonce),
        plaintext, sizeof(plaintext), aad, sizeof(aad), &sealed) == H2_PAL_OK);
    assert(h2_pal_crypto_aead_open(
        api, mode, key, key_len, nonce, sizeof(nonce),
        sealed.data, sealed.len, aad, sizeof(aad), &plain) == H2_PAL_OK);
    assert(plain.len == sizeof(plaintext));
    assert(memcmp(plain.data, plaintext, sizeof(plaintext)) == 0);

    uint8_t inplace[sizeof(plaintext) + 16u];
    memcpy(inplace, plaintext, sizeof(plaintext));
    h2_pal_crypto_buf_t inplace_buffer = {
        .data = inplace,
        .cap = sizeof(inplace),
    };
    assert(h2_pal_crypto_aead_seal(
        api, mode, key, key_len, nonce, sizeof(nonce),
        inplace, sizeof(plaintext), aad, sizeof(aad),
        &inplace_buffer) == H2_PAL_OK);
    assert(inplace_buffer.len == sizeof(inplace));
    assert(h2_pal_crypto_aead_open(
        api, mode, key, key_len, nonce, sizeof(nonce),
        inplace, inplace_buffer.len, aad, sizeof(aad),
        &inplace_buffer) == H2_PAL_OK);
    assert(inplace_buffer.len == sizeof(plaintext));
    assert(memcmp(inplace, plaintext, sizeof(plaintext)) == 0);

    uint8_t overlap[sizeof(plaintext) + 17u];
    memcpy(overlap, plaintext, sizeof(plaintext));
    h2_pal_crypto_buf_t overlap_buffer = {
        .data = overlap + 1u,
        .len = 123u,
        .cap = sizeof(overlap) - 1u,
    };
    assert(h2_pal_crypto_aead_seal(
        api, mode, key, key_len, nonce, sizeof(nonce),
        overlap, sizeof(plaintext), aad, sizeof(aad),
        &overlap_buffer) == H2_PAL_ERR_INVALID_ARG);
    assert(overlap_buffer.len == 0u);
    memcpy(overlap, ciphertext, sealed.len);
    overlap_buffer.len = 123u;
    assert(h2_pal_crypto_aead_open(
        api, mode, key, key_len, nonce, sizeof(nonce),
        overlap, sealed.len, aad, sizeof(aad),
        &overlap_buffer) == H2_PAL_ERR_INVALID_ARG);
    assert(overlap_buffer.len == 0u);

    ciphertext[0] ^= 1u;
    assert(h2_pal_crypto_aead_open(
        api, mode, key, key_len, nonce, sizeof(nonce),
        sealed.data, sealed.len, aad, sizeof(aad), &plain) == H2_PAL_ERR_FORMAT);
    assert(plain.len == 0u);
    size_t index;
    for (index = 0u; index < sizeof(opened); ++index) {
        assert(opened[index] == 0u);
    }
    ciphertext[0] ^= 1u;
    uint8_t modified_aad[sizeof(aad)];
    memcpy(modified_aad, aad, sizeof(aad));
    modified_aad[0] ^= 1u;
    assert(h2_pal_crypto_aead_open(
        api, mode, key, key_len, nonce, sizeof(nonce),
        sealed.data, sealed.len, modified_aad, sizeof(modified_aad),
        &plain) == H2_PAL_ERR_FORMAT);
    ciphertext[sealed.len - 1u] ^= 1u;
    assert(h2_pal_crypto_aead_open(
        api, mode, key, key_len, nonce, sizeof(nonce),
        sealed.data, sealed.len, aad, sizeof(aad), &plain) == H2_PAL_ERR_FORMAT);
    ciphertext[sealed.len - 1u] ^= 1u;
    assert(h2_pal_crypto_aead_open(
        api, mode, key, key_len, nonce, sizeof(nonce),
        sealed.data, 15u, aad, sizeof(aad), &plain) == H2_PAL_ERR_FORMAT);
}

static void test_aes_ctr(const h2_pal_crypto_api_t *api) {
    static const uint8_t counter[16] = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
        0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
    };
    static const uint8_t plaintext[16] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    };
    static const uint8_t keys[3][32] = {
        {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
         0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c},
        {0x8e, 0x73, 0xb0, 0xf7, 0xda, 0x0e, 0x64, 0x52,
         0xc8, 0x10, 0xf3, 0x2b, 0x80, 0x90, 0x79, 0xe5,
         0x62, 0xf8, 0xea, 0xd2, 0x52, 0x2c, 0x6b, 0x7b},
        {0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
         0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
         0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
         0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4},
    };
    static const size_t key_lengths[3] = {16u, 24u, 32u};
    static const uint8_t expected[3][16] = {
        {0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26,
         0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce},
        {0x1a, 0xbc, 0x93, 0x24, 0x17, 0x52, 0x1c, 0xa2,
         0x4f, 0x2b, 0x04, 0x59, 0xfe, 0x7e, 0x6e, 0x0b},
        {0x60, 0x1e, 0xc3, 0x13, 0x77, 0x57, 0x89, 0xa5,
         0xb7, 0xa7, 0xf5, 0x04, 0xbb, 0xf3, 0xd2, 0x28},
    };
    uint8_t output[17];
    for (size_t index = 0u; index < 3u; ++index) {
        h2_pal_crypto_buf_t buffer = {
            .data = output,
            .cap = sizeof(output),
        };
        assert(h2_pal_crypto_aes_ctr_xor(
                   api, keys[index], key_lengths[index], counter,
                   plaintext, sizeof(plaintext), &buffer) == H2_PAL_OK);
        assert(buffer.len == sizeof(plaintext));
        assert(memcmp(output, expected[index], sizeof(plaintext)) == 0);
    }
    memcpy(output, plaintext, sizeof(plaintext));
    h2_pal_crypto_buf_t in_place = {
        .data = output,
        .cap = sizeof(plaintext),
    };
    assert(h2_pal_crypto_aes_ctr_xor(
               api, keys[0], 16u, counter, output, sizeof(plaintext),
               &in_place) == H2_PAL_OK);
    assert(memcmp(output, expected[0], sizeof(plaintext)) == 0);
    h2_pal_crypto_buf_t overlap = {
        .data = output + 1u,
        .cap = sizeof(plaintext),
    };
    assert(h2_pal_crypto_aes_ctr_xor(
               api, keys[0], 16u, counter, output, sizeof(plaintext),
               &overlap) == H2_PAL_ERR_INVALID_ARG);
    uint8_t final_counter[16];
    memset(final_counter, 0xff, sizeof(final_counter));
    h2_pal_crypto_buf_t overflow = {
        .data = output,
        .cap = sizeof(output),
    };
    assert(h2_pal_crypto_aes_ctr_xor(
               api, keys[0], 16u, final_counter, plaintext, 17u,
               &overflow) == H2_PAL_ERR_INVALID_ARG);
}

static void test_compatibility_hashes(const h2_pal_crypto_api_t *api) {
    static const uint8_t md5_expected[16] = {
        0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
        0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72,
    };
    static const uint8_t hmac_expected[20] = {
        0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64,
        0xe2, 0x8b, 0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e,
        0xf1, 0x46, 0xbe, 0x00,
    };
    const uint8_t input[] = "abc";
    uint8_t digest[H2_PAL_CRYPTO_SHA1_SIZE];
    assert(h2_pal_crypto_md5(
               api, input, sizeof(input) - 1u, digest) == H2_PAL_OK);
    assert(memcmp(digest, md5_expected, sizeof(md5_expected)) == 0);
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    const uint8_t message[] = "Hi There";
    assert(h2_pal_crypto_hmac_sha1(
               api, key, sizeof(key), message, sizeof(message) - 1u,
               digest) == H2_PAL_OK);
    assert(memcmp(digest, hmac_expected, sizeof(hmac_expected)) == 0);
}

int main(void) {
    static const uint8_t aes128_empty_tag[16] = {
        0x58, 0xe2, 0xfc, 0xce, 0xfa, 0x7e, 0x30, 0x61,
        0x36, 0x7f, 0x1d, 0x57, 0xa4, 0xe7, 0x45, 0x5a,
    };
    static const uint8_t aes256_empty_tag[16] = {
        0x53, 0x0f, 0x8a, 0xfb, 0xc7, 0x45, 0x36, 0xb9,
        0xa9, 0x63, 0xb4, 0xf1, 0xc4, 0xcb, 0x73, 0x8b,
    };
    static const uint8_t p256_generator_compressed[33] = {
        0x03, 0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42,
        0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40,
        0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33,
        0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2,
        0x96,
    };
    static const uint8_t p256_signature_kat[64] = {
        0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47,
        0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
        0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
        0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
        0x5d, 0xb5, 0x36, 0xe5, 0x73, 0xcc, 0x94, 0x61,
        0x3d, 0x74, 0x94, 0x10, 0x8a, 0x2b, 0x43, 0x86,
        0x8c, 0x80, 0x9a, 0xa3, 0xb0, 0x46, 0x89, 0x49,
        0x9d, 0x34, 0x26, 0xa1, 0x9e, 0xd9, 0x9b, 0x68,
    };
    uint32_t entropy_state = 1u;
    h2_wolfcrypt_crypto_config_t config = {
        .entropy_user = &entropy_state,
        .entropy = test_entropy,
    };
    h2_pal_p256_keypair_t p256;
    h2_pal_p256_signature_t signature;
    h2_pal_p256_private_key_t invalid_private = {0};
    h2_pal_p256_public_key_t invalid_public = {0};
    const uint8_t message[] = "P-256 test";
    uint8_t random[32];

    h2_wolfcrypt_crypto_deinit();
    assert(h2_wolfcrypt_crypto_init(&config) == H2_PAL_OK);
    const h2_pal_crypto_api_t *api = h2_wolfcrypt_crypto_api();

    assert(h2_pal_crypto_random(api, random, sizeof(random)) == H2_PAL_OK);
    assert(api->vtable->random(api->user, NULL, 1u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->random(
               api->user, random, (size_t)INT_MAX + 1u) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_pal_crypto_buf_t oversized_output = {
        .data = random,
        .cap = sizeof(random),
    };
    assert(api->vtable->hkdf_sha256(
               api->user, random, 1u, random, (size_t)INT_MAX + 1u,
               NULL, 0u, random, 1u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->aead_seal(
               api->user, H2_PAL_CRYPTO_AEAD_AES_256_GCM, random, 32u,
               random, 12u, random, (size_t)INT_MAX, NULL, 0u,
               &oversized_output) == H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->aead_open(
               api->user, H2_PAL_CRYPTO_AEAD_AES_256_GCM, random, 32u,
               random, 12u, random, (size_t)INT_MAX + 1u, NULL, 0u,
               &oversized_output) == H2_PAL_ERR_INVALID_ARG);
    test_direct_vtable_null_arguments(api);
    test_x25519(api);
    test_hkdf(api);
    test_aead(api, H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305, 32u);
    test_chacha20_poly1305_kat(api);
    test_aead(api, H2_PAL_CRYPTO_AEAD_AES_128_GCM, 16u);
    test_aead(api, H2_PAL_CRYPTO_AEAD_AES_256_GCM, 32u);
    test_aes_ctr(api);
    test_compatibility_hashes(api);
    test_aes_empty_kat(
        api, H2_PAL_CRYPTO_AEAD_AES_128_GCM, 16u, aes128_empty_tag);
    test_aes_empty_kat(
        api, H2_PAL_CRYPTO_AEAD_AES_256_GCM, 32u, aes256_empty_tag);
    h2_pal_crypto_buf_t too_small = {
        .data = random,
        .cap = 1u,
    };
    assert(h2_pal_crypto_aead_seal(
        api, H2_PAL_CRYPTO_AEAD_AES_256_GCM, random, 32u,
        random, 12u, random, 1u, NULL, 0u,
        &too_small) == H2_PAL_ERR_NO_SPACE);
    assert(h2_pal_crypto_aead_seal(
        api, (h2_pal_crypto_aead_algorithm_t)99, random, 32u,
        random, 12u, NULL, 0u, NULL, 0u,
        &too_small) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_crypto_aead_seal(
        api, H2_PAL_CRYPTO_AEAD_AES_256_GCM, random, 31u,
        random, 12u, NULL, 0u, NULL, 0u,
        &too_small) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_crypto_aead_seal(
        api, H2_PAL_CRYPTO_AEAD_AES_256_GCM, random, 32u,
        random, 11u, NULL, 0u, NULL, 0u,
        &too_small) == H2_PAL_ERR_INVALID_ARG);
    too_small.len = 123u;
    assert(h2_pal_crypto_aead_seal(
        api, H2_PAL_CRYPTO_AEAD_AES_256_GCM, random, 32u,
        random, 12u, random, SIZE_MAX, NULL, 0u,
        &too_small) == H2_PAL_ERR_INVALID_ARG);
    assert(too_small.len == 0u);
    assert(h2_pal_crypto_hkdf_sha256(
        api, NULL, 0u, NULL, 0u, NULL, 0u,
        random, 255u * 32u + 1u) == H2_PAL_ERR_INVALID_ARG);

    memset(&p256, 0, sizeof(p256));
    p256.private_key.bytes[31] = 1u;
    assert(h2_pal_crypto_p256_keypair_from_private(
        api, &p256.private_key, &p256) == H2_PAL_OK);
    assert(memcmp(
        p256.public_key.bytes, p256_generator_compressed,
        sizeof(p256_generator_compressed)) == 0);
    memcpy(signature.bytes, p256_signature_kat, sizeof(p256_signature_kat));
    assert(h2_pal_crypto_ecdsa_p256_sha256_verify(
        api, &p256.public_key, message, sizeof(message),
        &signature) == H2_PAL_OK);
    assert(h2_pal_crypto_p256_keypair_generate(api, &p256) == H2_PAL_OK);
    assert(h2_pal_crypto_p256_public_key_validate(
        api, &p256.public_key) == H2_PAL_OK);
    memset(&signature, 0xa5, sizeof(signature));
    assert(h2_pal_crypto_ecdsa_p256_sha256_sign(
        api, &p256.private_key, message, SIZE_MAX,
        &signature) == H2_PAL_ERR_INVALID_ARG);
    assert(memcmp(
        signature.bytes, (uint8_t[sizeof(signature.bytes)]){0},
        sizeof(signature.bytes)) == 0);
    assert(h2_pal_crypto_ecdsa_p256_sha256_sign(
        api, &p256.private_key, message, sizeof(message), &signature) == H2_PAL_OK);
    assert(h2_pal_crypto_ecdsa_p256_sha256_verify(
        api, &p256.public_key, message, sizeof(message), &signature) == H2_PAL_OK);
    signature.bytes[0] ^= 1u;
    assert(h2_pal_crypto_ecdsa_p256_sha256_verify(
        api, &p256.public_key, message, sizeof(message),
        &signature) == H2_PAL_ERR_FORMAT);
    static const uint8_t p256_order[32] = {
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
        0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
    };
    memset(&signature, 0, sizeof(signature));
    memcpy(signature.bytes, p256_order, sizeof(p256_order));
    signature.bytes[63] = 1u;
    assert(h2_pal_crypto_ecdsa_p256_sha256_verify(
        api, &p256.public_key, message, sizeof(message),
        &signature) == H2_PAL_ERR_FORMAT);
    memset(&signature, 0, sizeof(signature));
    signature.bytes[31] = 1u;
    memcpy(signature.bytes + 32u, p256_order, sizeof(p256_order));
    assert(h2_pal_crypto_ecdsa_p256_sha256_verify(
        api, &p256.public_key, message, sizeof(message),
        &signature) == H2_PAL_ERR_FORMAT);
    memset(&signature, 0, sizeof(signature));
    assert(h2_pal_crypto_ecdsa_p256_sha256_verify(
        api, &p256.public_key, message, sizeof(message),
        &signature) == H2_PAL_ERR_FORMAT);
    assert(h2_pal_crypto_p256_keypair_from_private(
        api, &invalid_private, &p256) == H2_PAL_ERR_FORMAT);
    invalid_public.bytes[0] = 0x04u;
    assert(h2_pal_crypto_p256_public_key_validate(
        api, &invalid_public) == H2_PAL_ERR_FORMAT);
    memset(&invalid_public, 0xff, sizeof(invalid_public));
    invalid_public.bytes[0] = 0x02u;
    assert(h2_pal_crypto_p256_public_key_validate(
        api, &invalid_public) == H2_PAL_ERR_FORMAT);

    h2_wolfcrypt_crypto_deinit();
    return 0;
}
