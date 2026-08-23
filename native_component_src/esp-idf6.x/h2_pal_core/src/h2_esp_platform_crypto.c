#include "h2_esp_platform_core.h"

#ifndef MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#endif
#include "esp_random.h"
#include <mbedtls/ecp.h>
#include <psa/crypto.h>

#include <string.h>

#define H2_PSA_AEAD_TAG_SIZE 16u
#define H2_PSA_NONCE_SIZE 12u

typedef struct h2_esp_platform_crypto_backend {
    const h2_pal_mem_api_t *allocator;
    void *random_user;
    h2_pal_result_t (*random)(void *user, uint8_t *out, size_t len);
} h2_esp_platform_crypto_backend_t;

static h2_pal_result_t h2_esp_crypto_random(void *user, uint8_t *out, size_t len) {
    (void)user;
    if (out == NULL && len > 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    esp_fill_random(out, len);
    return H2_PAL_OK;
}

static int h2_psa_to_platform(psa_status_t status) {
    switch (status) {
    case PSA_SUCCESS:
        return H2_PAL_OK;
    case PSA_ERROR_NOT_SUPPORTED:
        return H2_PAL_ERR_UNSUPPORTED;
    case PSA_ERROR_NOT_PERMITTED:
    case PSA_ERROR_INVALID_ARGUMENT:
    case PSA_ERROR_INVALID_HANDLE:
        return H2_PAL_ERR_INVALID_ARG;
    case PSA_ERROR_BUFFER_TOO_SMALL:
        return H2_PAL_ERR_NO_SPACE;
    case PSA_ERROR_INSUFFICIENT_MEMORY:
        return H2_PAL_ERR_NO_MEMORY;
#ifdef PSA_ERROR_INVALID_SIGNATURE
    case PSA_ERROR_INVALID_SIGNATURE:
        return H2_PAL_ERR_FORMAT;
#endif
    default:
        return H2_PAL_ERR_IO;
    }
}

static int h2_psa_init(void) {
    psa_status_t status = psa_crypto_init();
    return status == PSA_SUCCESS ? H2_PAL_OK : h2_psa_to_platform(status);
}

static int h2_p256_public_key_uncompress(
    const h2_pal_p256_public_key_t *public_key,
    uint8_t out[65]) {
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    int ret = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (ret == 0) {
        ret = mbedtls_ecp_point_read_binary(
            &group, &point, public_key->bytes, sizeof(public_key->bytes));
    }
    if (ret == 0) {
        ret = mbedtls_ecp_check_pubkey(&group, &point);
    }
    size_t out_len = 0u;
    if (ret == 0) {
        ret = mbedtls_ecp_point_write_binary(
            &group, &point, MBEDTLS_ECP_PF_UNCOMPRESSED, &out_len, out, 65u);
    }
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    if (ret == MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (ret == MBEDTLS_ERR_MPI_ALLOC_FAILED) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (ret != 0 || out_len != 65u) {
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

static h2_esp_platform_crypto_backend_t *h2_crypto_backend(void *user) {
    return (h2_esp_platform_crypto_backend_t *)user;
}

static h2_pal_result_t h2_mbedtls_crypto_random(void *user, uint8_t *out, size_t len) {
    h2_esp_platform_crypto_backend_t *backend = h2_crypto_backend(user);
    if (out == NULL && len > 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (backend != NULL && backend->random != NULL) {
        return backend->random(backend->random_user, out, len);
    }
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_psa_to_platform(psa_generate_random(out, len));
}

static void h2_clamp_x25519_private(
    uint8_t out[H2_PAL_CRYPTO_X25519_KEY_SIZE],
    const h2_pal_x25519_private_key_t *private_key) {
    memcpy(out, private_key->bytes, H2_PAL_CRYPTO_X25519_KEY_SIZE);
    out[0] &= 248u;
    out[31] &= 127u;
    out[31] |= 64u;
}

static int h2_import_x25519_private(
    const h2_pal_x25519_private_key_t *private_key,
    psa_key_usage_t usage,
    mbedtls_svc_key_id_t *out_key) {
    uint8_t clamped[H2_PAL_CRYPTO_X25519_KEY_SIZE];
    h2_clamp_x25519_private(clamped, private_key);

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attributes, 255u);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);

    psa_status_t status = psa_import_key(&attributes, clamped, sizeof(clamped), out_key);
    psa_reset_key_attributes(&attributes);
    return h2_psa_to_platform(status);
}

static h2_pal_result_t h2_mbedtls_crypto_x25519_public_key_from_private(
    void *user,
    const h2_pal_x25519_private_key_t *private_key,
    h2_pal_x25519_public_key_t *out_public_key) {
    (void)user;
    if (private_key == NULL || out_public_key == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_public_key, 0, sizeof(*out_public_key));
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    rc = h2_import_x25519_private(private_key, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT, &key);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    size_t public_len = 0u;
    psa_status_t status = psa_export_public_key(
        key, out_public_key->bytes, sizeof(out_public_key->bytes), &public_len);
    psa_destroy_key(key);
    if (status != PSA_SUCCESS) {
        return h2_psa_to_platform(status);
    }
    if (public_len != H2_PAL_CRYPTO_X25519_KEY_SIZE) {
        memset(out_public_key, 0, sizeof(*out_public_key));
        return H2_PAL_ERR_FORMAT;
    }
    out_public_key->bytes[31] &= 0x7fu;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_x25519_keypair_generate(
    void *user,
    h2_pal_x25519_keypair_t *out_keypair) {
    if (out_keypair == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_keypair, 0, sizeof(*out_keypair));
    int rc = h2_mbedtls_crypto_random(
        user, out_keypair->private_key.bytes,
        sizeof(out_keypair->private_key.bytes));
    if (rc == H2_PAL_OK) {
        rc = h2_mbedtls_crypto_x25519_public_key_from_private(
            user, &out_keypair->private_key, &out_keypair->public_key);
    }
    if (rc != H2_PAL_OK) {
        memset(out_keypair, 0, sizeof(*out_keypair));
    }
    return rc;
}

static h2_pal_result_t h2_mbedtls_crypto_x25519_shared_secret(
    void *user,
    const h2_pal_x25519_private_key_t *local_private_key,
    const h2_pal_x25519_public_key_t *remote_public_key,
    h2_pal_x25519_shared_secret_t *out_shared_secret) {
    (void)user;
    if (local_private_key == NULL || remote_public_key == NULL ||
        out_shared_secret == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_shared_secret, 0, sizeof(*out_shared_secret));
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    rc = h2_import_x25519_private(
        local_private_key, PSA_KEY_USAGE_DERIVE, &key);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    uint8_t remote_copy[H2_PAL_CRYPTO_X25519_KEY_SIZE];
    memcpy(remote_copy, remote_public_key->bytes, sizeof(remote_copy));
    remote_copy[31] &= 0x7fu;
    size_t shared_len = 0u;
    psa_status_t status = psa_raw_key_agreement(
        PSA_ALG_ECDH,
        key,
        remote_copy,
        sizeof(remote_copy),
        out_shared_secret->bytes,
        sizeof(out_shared_secret->bytes),
        &shared_len);
    memset(remote_copy, 0, sizeof(remote_copy));
    psa_destroy_key(key);
    if (status != PSA_SUCCESS) {
        return h2_psa_to_platform(status);
    }
    if (shared_len != H2_PAL_CRYPTO_X25519_KEY_SIZE) {
        memset(out_shared_secret, 0, sizeof(*out_shared_secret));
        return H2_PAL_ERR_FORMAT;
    }
    uint8_t any = 0u;
    for (size_t i = 0u; i < H2_PAL_CRYPTO_X25519_KEY_SIZE; ++i) {
        any |= out_shared_secret->bytes[i];
    }
    if (any == 0u) {
        memset(out_shared_secret, 0, sizeof(*out_shared_secret));
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_hkdf_sha256(
    void *user,
    const uint8_t *secret,
    size_t secret_len,
    const uint8_t *salt,
    size_t salt_len,
    const uint8_t *info,
    size_t info_len,
    uint8_t *out,
    size_t out_len) {
    static const uint8_t empty = 0u;
    (void)user;
    if ((secret == NULL && secret_len > 0u) || (salt == NULL && salt_len > 0u) ||
        (info == NULL && info_len > 0u) || (out == NULL && out_len > 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (out_len > 255u * 32u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    psa_key_derivation_operation_t operation =
        PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(
        &operation, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_SALT,
            salt_len == 0u ? &empty : salt, salt_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_SECRET,
            secret_len == 0u ? &empty : secret, secret_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_INFO,
            info_len == 0u ? &empty : info, info_len);
    }
    if (status == PSA_SUCCESS && out_len != 0u) {
        status = psa_key_derivation_output_bytes(&operation, out, out_len);
    }
    (void)psa_key_derivation_abort(&operation);
    if (status != PSA_SUCCESS) {
        if (out != NULL) {
            memset(out, 0, out_len);
        }
        return h2_psa_to_platform(status);
    }
    return H2_PAL_OK;
}

static psa_algorithm_t h2_psa_aead_alg(
    h2_pal_crypto_aead_algorithm_t algorithm) {
    switch (algorithm) {
    case H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305:
        return PSA_ALG_CHACHA20_POLY1305;
    case H2_PAL_CRYPTO_AEAD_AES_128_GCM:
    case H2_PAL_CRYPTO_AEAD_AES_256_GCM:
        return PSA_ALG_GCM;
    default:
        return 0;
    }
}

static psa_key_type_t h2_psa_aead_key_type(
    h2_pal_crypto_aead_algorithm_t algorithm) {
    switch (algorithm) {
    case H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305:
        return PSA_KEY_TYPE_CHACHA20;
    case H2_PAL_CRYPTO_AEAD_AES_128_GCM:
    case H2_PAL_CRYPTO_AEAD_AES_256_GCM:
        return PSA_KEY_TYPE_AES;
    default:
        return 0;
    }
}

static int h2_crypto_prepare_out(
    h2_esp_platform_crypto_backend_t *backend,
    h2_pal_crypto_buf_t *out,
    size_t needed) {
    (void)backend;
    if (out == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out->len = 0u;
    if (out->data == NULL && needed > 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (out->cap < needed) {
        return H2_PAL_ERR_NO_SPACE;
    }
    return H2_PAL_OK;
}

static int h2_psa_import_aead_key(
    h2_pal_crypto_aead_algorithm_t algorithm,
    const uint8_t *key_data,
    size_t key_len,
    psa_key_usage_t usage,
    mbedtls_svc_key_id_t *out_key) {
    psa_algorithm_t alg = h2_psa_aead_alg(algorithm);
    psa_key_type_t type = h2_psa_aead_key_type(algorithm);
    if (alg == 0 || type == 0) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (key_data == NULL || key_len == 0u || out_key == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if ((algorithm == H2_PAL_CRYPTO_AEAD_AES_128_GCM && key_len != 16u) ||
        (algorithm != H2_PAL_CRYPTO_AEAD_AES_128_GCM && key_len != 32u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, type);
    psa_set_key_bits(&attributes, key_len * 8u);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, alg);
    psa_status_t status = psa_import_key(&attributes, key_data, key_len, out_key);
    psa_reset_key_attributes(&attributes);
    return h2_psa_to_platform(status);
}

static int h2_crypto_nonexact_overlap(
    const uint8_t *input,
    size_t input_len,
    uint8_t *output,
    size_t output_len);

static h2_pal_result_t h2_mbedtls_crypto_aead_seal(
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
    h2_pal_crypto_buf_t *out_ciphertext) {
    h2_esp_platform_crypto_backend_t *backend = h2_crypto_backend(user);
    if (out_ciphertext != NULL) {
        out_ciphertext->len = 0u;
    }
    if ((plaintext == NULL && plaintext_len > 0u) || (aad == NULL && aad_len > 0u) ||
        nonce == NULL || nonce_len != H2_PSA_NONCE_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_psa_aead_alg(algorithm) == 0) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (plaintext_len > SIZE_MAX - H2_PSA_AEAD_TAG_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const size_t needed = plaintext_len + H2_PSA_AEAD_TAG_SIZE;
    rc = h2_crypto_prepare_out(backend, out_ciphertext, needed);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (h2_crypto_nonexact_overlap(
            plaintext, plaintext_len, out_ciphertext->data, needed)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    mbedtls_svc_key_id_t imported_key = MBEDTLS_SVC_KEY_ID_INIT;
    rc = h2_psa_import_aead_key(
        algorithm, key, key_len, PSA_KEY_USAGE_ENCRYPT, &imported_key);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    size_t out_len = 0u;
    const uint8_t empty_input = 0u;
    const uint8_t *psa_plaintext = plaintext_len > 0u ? plaintext : &empty_input;
    const uint8_t *psa_aad = aad_len > 0u ? aad : &empty_input;
    psa_status_t status = psa_aead_encrypt(
        imported_key,
        h2_psa_aead_alg(algorithm),
        nonce,
        nonce_len,
        psa_aad,
        aad_len,
        psa_plaintext,
        plaintext_len,
        out_ciphertext->data,
        out_ciphertext->cap,
        &out_len);
    psa_destroy_key(imported_key);
    if (status != PSA_SUCCESS) {
        if (needed > 0u) {
            memset(out_ciphertext->data, 0, needed);
        }
        return h2_psa_to_platform(status);
    }
    out_ciphertext->len = out_len;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_aead_open(
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
    h2_pal_crypto_buf_t *out_plaintext) {
    h2_esp_platform_crypto_backend_t *backend = h2_crypto_backend(user);
    if (out_plaintext != NULL) {
        out_plaintext->len = 0u;
    }
    if ((ciphertext == NULL && ciphertext_len > 0u) || (aad == NULL && aad_len > 0u) ||
        nonce == NULL || nonce_len != H2_PSA_NONCE_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_psa_aead_alg(algorithm) == 0) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (ciphertext_len < H2_PSA_AEAD_TAG_SIZE) {
        return H2_PAL_ERR_FORMAT;
    }

    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_crypto_prepare_out(backend, out_plaintext, ciphertext_len - H2_PSA_AEAD_TAG_SIZE);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (h2_crypto_nonexact_overlap(
            ciphertext, ciphertext_len, out_plaintext->data,
            ciphertext_len - H2_PSA_AEAD_TAG_SIZE)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    mbedtls_svc_key_id_t imported_key = MBEDTLS_SVC_KEY_ID_INIT;
    rc = h2_psa_import_aead_key(
        algorithm, key, key_len, PSA_KEY_USAGE_DECRYPT, &imported_key);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    size_t out_len = 0u;
    const uint8_t empty_input = 0u;
    uint8_t empty_output = 0u;
    const uint8_t *psa_aad = aad_len > 0u ? aad : &empty_input;
    uint8_t *psa_output = out_plaintext->data != NULL ? out_plaintext->data : &empty_output;
    psa_status_t status = psa_aead_decrypt(
        imported_key,
        h2_psa_aead_alg(algorithm),
        nonce,
        nonce_len,
        psa_aad,
        aad_len,
        ciphertext,
        ciphertext_len,
        psa_output,
        out_plaintext->cap,
        &out_len);
    psa_destroy_key(imported_key);
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        if (ciphertext_len > H2_PSA_AEAD_TAG_SIZE) {
            memset(out_plaintext->data, 0, ciphertext_len - H2_PSA_AEAD_TAG_SIZE);
        }
        return H2_PAL_ERR_FORMAT;
    }
    if (status != PSA_SUCCESS) {
        if (ciphertext_len > H2_PSA_AEAD_TAG_SIZE) {
            memset(out_plaintext->data, 0, ciphertext_len - H2_PSA_AEAD_TAG_SIZE);
        }
        return h2_psa_to_platform(status);
    }
    out_plaintext->len = out_len;
    return H2_PAL_OK;
}

static int h2_crypto_counter_has_capacity(
    const uint8_t counter[H2_PAL_CRYPTO_AES_BLOCK_SIZE],
    size_t input_len) {
    size_t blocks = input_len / H2_PAL_CRYPTO_AES_BLOCK_SIZE;
    size_t increment;
    size_t index;
    if ((input_len % H2_PAL_CRYPTO_AES_BLOCK_SIZE) != 0u) {
        ++blocks;
    }
    if (blocks == 0u) {
        return 1;
    }
    increment = blocks - 1u;
    for (index = H2_PAL_CRYPTO_AES_BLOCK_SIZE;
         index > 0u && increment != 0u; --index) {
        unsigned int sum = (unsigned int)counter[index - 1u] +
                           (unsigned int)(increment & 0xffu);
        increment >>= 8u;
        if (sum > 0xffu) {
            ++increment;
        }
    }
    return increment == 0u;
}

static int h2_crypto_nonexact_overlap(
    const uint8_t *input,
    size_t input_len,
    uint8_t *output,
    size_t output_len) {
    uintptr_t input_address;
    uintptr_t output_address;
    if (input_len == 0u || output_len == 0u || input == output) {
        return 0;
    }
    input_address = (uintptr_t)input;
    output_address = (uintptr_t)output;
    if (input_address > UINTPTR_MAX - input_len ||
        output_address > UINTPTR_MAX - output_len) {
        return 1;
    }
    return input_address < output_address + output_len &&
           output_address < input_address + input_len;
}

static h2_pal_result_t h2_mbedtls_crypto_aes_ctr_xor(
    void *user,
    const uint8_t *key,
    size_t key_len,
    const uint8_t initial_counter[H2_PAL_CRYPTO_AES_BLOCK_SIZE],
    const uint8_t *input,
    size_t input_len,
    h2_pal_crypto_buf_t *out) {
    h2_esp_platform_crypto_backend_t *backend = h2_crypto_backend(user);
    if (out == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out->len = 0u;
    if (key == NULL || initial_counter == NULL ||
        (input == NULL && input_len != 0u) ||
        (key_len != 16u && key_len != 24u && key_len != 32u) ||
        !h2_crypto_counter_has_capacity(initial_counter, input_len) ||
        h2_crypto_nonexact_overlap(
            input, input_len, out->data, input_len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_crypto_prepare_out(backend, out, input_len);
    if (rc != H2_PAL_OK || input_len == 0u) {
        return rc;
    }
    rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key_len * 8u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
    mbedtls_svc_key_id_t imported_key = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(
        &attributes, key, key_len, &imported_key);
    psa_reset_key_attributes(&attributes);
    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_encrypt_setup(
            &operation, imported_key, PSA_ALG_CTR);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(
            &operation, initial_counter, H2_PAL_CRYPTO_AES_BLOCK_SIZE);
    }
    size_t update_len = 0u;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(
            &operation, input, input_len, out->data, out->cap, &update_len);
    }
    size_t finish_len = 0u;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(
            &operation, out->data + update_len, out->cap - update_len,
            &finish_len);
    }
    (void)psa_cipher_abort(&operation);
    (void)psa_destroy_key(imported_key);
    if (status != PSA_SUCCESS || update_len + finish_len != input_len) {
        memset(out->data, 0, input_len);
        return status == PSA_SUCCESS
                   ? H2_PAL_ERR_IO
                   : h2_psa_to_platform(status);
    }
    out->len = input_len;
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_md5(
    void *user,
    const uint8_t *input,
    size_t input_len,
    uint8_t out_digest[H2_PAL_CRYPTO_MD5_SIZE]) {
    static const uint8_t empty = 0u;
    (void)user;
    if ((input == NULL && input_len != 0u) || out_digest == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_digest, 0, H2_PAL_CRYPTO_MD5_SIZE);
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    size_t digest_len = 0u;
    psa_status_t status = psa_hash_compute(
        PSA_ALG_MD5, input_len == 0u ? &empty : input, input_len,
        out_digest, H2_PAL_CRYPTO_MD5_SIZE, &digest_len);
    if (status != PSA_SUCCESS || digest_len != H2_PAL_CRYPTO_MD5_SIZE) {
        memset(out_digest, 0, H2_PAL_CRYPTO_MD5_SIZE);
        return status == PSA_SUCCESS
                   ? H2_PAL_ERR_IO
                   : h2_psa_to_platform(status);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_hmac_sha1(
    void *user,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *input,
    size_t input_len,
    uint8_t out_digest[H2_PAL_CRYPTO_SHA1_SIZE]) {
    static const uint8_t empty = 0u;
    (void)user;
    if ((key == NULL && key_len != 0u) ||
        (input == NULL && input_len != 0u) || out_digest == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_digest, 0, H2_PAL_CRYPTO_SHA1_SIZE);
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, key_len * 8u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_1));
    mbedtls_svc_key_id_t imported_key = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(
        &attributes, key_len == 0u ? &empty : key, key_len, &imported_key);
    psa_reset_key_attributes(&attributes);
    size_t digest_len = 0u;
    if (status == PSA_SUCCESS) {
        status = psa_mac_compute(
            imported_key, PSA_ALG_HMAC(PSA_ALG_SHA_1),
            input_len == 0u ? &empty : input, input_len,
            out_digest, H2_PAL_CRYPTO_SHA1_SIZE, &digest_len);
    }
    (void)psa_destroy_key(imported_key);
    if (status != PSA_SUCCESS || digest_len != H2_PAL_CRYPTO_SHA1_SIZE) {
        memset(out_digest, 0, H2_PAL_CRYPTO_SHA1_SIZE);
        return status == PSA_SUCCESS
                   ? H2_PAL_ERR_IO
                   : h2_psa_to_platform(status);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_p256_keypair_from_private(
    void *user,
    const h2_pal_p256_private_key_t *private_key,
    h2_pal_p256_keypair_t *out_keypair) {
    (void)user;
    if (private_key == NULL || out_keypair == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_keypair, 0, sizeof(*out_keypair));
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(
        &attributes, private_key->bytes, sizeof(private_key->bytes), &key);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return status == PSA_ERROR_INVALID_ARGUMENT ? H2_PAL_ERR_FORMAT : h2_psa_to_platform(status);
    }

    uint8_t public_key[65];
    size_t public_len = 0u;
    status = psa_export_public_key(key, public_key, sizeof(public_key), &public_len);
    psa_destroy_key(key);
    if (status != PSA_SUCCESS) {
        return h2_psa_to_platform(status);
    }
    if (public_len != sizeof(public_key) || public_key[0] != 0x04u) {
        return H2_PAL_ERR_FORMAT;
    }
    memcpy(out_keypair->private_key.bytes, private_key->bytes, sizeof(private_key->bytes));
    out_keypair->public_key.bytes[0] = (uint8_t)(0x02u | (public_key[64] & 1u));
    memcpy(out_keypair->public_key.bytes + 1u, public_key + 1u, 32u);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_p256_keypair_generate(
    void *user,
    h2_pal_p256_keypair_t *out_keypair) {
    (void)user;
    if (out_keypair == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_keypair, 0, sizeof(*out_keypair));
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_generate_key(&attributes, &key);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return h2_psa_to_platform(status);
    }

    size_t private_len = 0u;
    status = psa_export_key(
        key, out_keypair->private_key.bytes, sizeof(out_keypair->private_key.bytes), &private_len);
    uint8_t public_key[65];
    size_t public_len = 0u;
    if (status == PSA_SUCCESS) {
        status = psa_export_public_key(key, public_key, sizeof(public_key), &public_len);
    }
    psa_destroy_key(key);
    if (status != PSA_SUCCESS) {
        memset(out_keypair, 0, sizeof(*out_keypair));
        return h2_psa_to_platform(status);
    }
    if (private_len != sizeof(out_keypair->private_key.bytes) ||
        public_len != sizeof(public_key) || public_key[0] != 0x04u) {
        memset(out_keypair, 0, sizeof(*out_keypair));
        return H2_PAL_ERR_FORMAT;
    }
    out_keypair->public_key.bytes[0] = (uint8_t)(0x02u | (public_key[64] & 1u));
    memcpy(out_keypair->public_key.bytes + 1u, public_key + 1u, 32u);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_p256_public_key_validate(
    void *user,
    const h2_pal_p256_public_key_t *public_key) {
    (void)user;
    if (public_key == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    uint8_t uncompressed[65];
    rc = h2_p256_public_key_uncompress(public_key, uncompressed);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(
        &attributes, uncompressed, sizeof(uncompressed), &key);
    psa_reset_key_attributes(&attributes);
    if (status == PSA_SUCCESS) {
        psa_destroy_key(key);
        return H2_PAL_OK;
    }
    return status == PSA_ERROR_INVALID_ARGUMENT ? H2_PAL_ERR_FORMAT : h2_psa_to_platform(status);
}

static h2_pal_result_t h2_mbedtls_crypto_ecdsa_p256_sha256_sign(
    void *user,
    const h2_pal_p256_private_key_t *private_key,
    const uint8_t *message,
    size_t message_len,
    h2_pal_p256_signature_t *out_signature) {
    (void)user;
    if (private_key == NULL || (message == NULL && message_len > 0u) ||
        out_signature == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_signature, 0, sizeof(*out_signature));
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(
        &attributes, private_key->bytes, sizeof(private_key->bytes), &key);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return status == PSA_ERROR_INVALID_ARGUMENT ? H2_PAL_ERR_FORMAT : h2_psa_to_platform(status);
    }

    const uint8_t empty_message = 0u;
    size_t signature_len = 0u;
    status = psa_sign_message(
        key,
        PSA_ALG_ECDSA(PSA_ALG_SHA_256),
        message_len > 0u ? message : &empty_message,
        message_len,
        out_signature->bytes,
        sizeof(out_signature->bytes),
        &signature_len);
    psa_destroy_key(key);
    if (status != PSA_SUCCESS) {
        memset(out_signature, 0, sizeof(*out_signature));
        return h2_psa_to_platform(status);
    }
    if (signature_len != sizeof(out_signature->bytes)) {
        memset(out_signature, 0, sizeof(*out_signature));
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_mbedtls_crypto_ecdsa_p256_sha256_verify(
    void *user,
    const h2_pal_p256_public_key_t *public_key,
    const uint8_t *message,
    size_t message_len,
    const h2_pal_p256_signature_t *signature) {
    (void)user;
    if (public_key == NULL || (message == NULL && message_len > 0u) ||
        signature == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_psa_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    uint8_t uncompressed[65];
    rc = h2_p256_public_key_uncompress(public_key, uncompressed);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256u);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(
        &attributes, uncompressed, sizeof(uncompressed), &key);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return status == PSA_ERROR_INVALID_ARGUMENT ? H2_PAL_ERR_FORMAT : h2_psa_to_platform(status);
    }

    const uint8_t empty_message = 0u;
    status = psa_verify_message(
        key,
        PSA_ALG_ECDSA(PSA_ALG_SHA_256),
        message_len > 0u ? message : &empty_message,
        message_len,
        signature->bytes,
        sizeof(signature->bytes));
    psa_destroy_key(key);
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        return H2_PAL_ERR_FORMAT;
    }
    return h2_psa_to_platform(status);
}

static void h2_esp_platform_crypto_init_api(
    h2_pal_crypto_api_t *api,
    h2_esp_platform_crypto_backend_t *backend) {
    if (api == NULL) {
        return;
    }
    static const h2_pal_crypto_vtable_t vtable = {
        .random = h2_mbedtls_crypto_random,
        .x25519_keypair_generate = h2_mbedtls_crypto_x25519_keypair_generate,
        .x25519_public_key_from_private = h2_mbedtls_crypto_x25519_public_key_from_private,
        .x25519_shared_secret = h2_mbedtls_crypto_x25519_shared_secret,
        .hkdf_sha256 = h2_mbedtls_crypto_hkdf_sha256,
        .aead_seal = h2_mbedtls_crypto_aead_seal,
        .aead_open = h2_mbedtls_crypto_aead_open,
        .aes_ctr_xor = h2_mbedtls_crypto_aes_ctr_xor,
        .md5 = h2_mbedtls_crypto_md5,
        .hmac_sha1 = h2_mbedtls_crypto_hmac_sha1,
        .p256_keypair_from_private = h2_mbedtls_crypto_p256_keypair_from_private,
        .p256_keypair_generate = h2_mbedtls_crypto_p256_keypair_generate,
        .p256_public_key_validate = h2_mbedtls_crypto_p256_public_key_validate,
        .ecdsa_p256_sha256_sign = h2_mbedtls_crypto_ecdsa_p256_sha256_sign,
        .ecdsa_p256_sha256_verify = h2_mbedtls_crypto_ecdsa_p256_sha256_verify,
    };
    api->user = backend;
    api->vtable = &vtable;
}

const h2_pal_crypto_api_t *h2_esp_platform_crypto_api(void) {
    static h2_esp_platform_crypto_backend_t backend;
    static h2_pal_crypto_api_t api;
    static int initialized;
    if (!initialized) {
        backend.allocator = h2_esp_platform_default_allocator();
        backend.random_user = NULL;
        backend.random = h2_esp_crypto_random;
        h2_esp_platform_crypto_init_api(&api, &backend);
        initialized = 1;
    }
    return &api;
}
