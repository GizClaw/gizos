#include "config.h"
#include "h2_libsrtp_crypto_internal.h"

#include "alloc.h"
#include "auth_test_cases.h"
#include "cipher_test_cases.h"
#include "cipher_types.h"
#include "datatypes.h"

#include <string.h>

srtp_debug_module_t srtp_mod_aes_icm = { 0, "h2 pal aes icm" };
srtp_debug_module_t srtp_mod_aes_gcm = { 0, "h2 pal aes gcm" };
srtp_debug_module_t srtp_mod_hmac = { 0, "h2 pal hmac sha1" };

static srtp_err_status_t h2_libsrtp_crypto_status(
    h2_pal_result_t result,
    srtp_err_status_t format_status) {
    if (result == H2_PAL_OK) {
        return srtp_err_status_ok;
    }
    h2_libsrtp_record_crypto_result(result);
    if (result == H2_PAL_ERR_FORMAT) {
        return format_status;
    }
    if (result == H2_PAL_ERR_NO_MEMORY) {
        return srtp_err_status_alloc_fail;
    }
    if (result == H2_PAL_ERR_INVALID_ARG ||
        result == H2_PAL_ERR_NO_SPACE || result == H2_PAL_ERR_TRUNCATED) {
        return srtp_err_status_bad_param;
    }
    return srtp_err_status_cipher_fail;
}

static void h2_libsrtp_increment_counter(uint8_t counter[16]) {
    size_t index = 16u;
    while (index > 0u) {
        --index;
        ++counter[index];
        if (counter[index] != 0u) {
            break;
        }
    }
}

static srtp_err_status_t h2_libsrtp_icm_alloc(
    srtp_cipher_t **out_cipher, int key_len, int tag_len) {
    srtp_cipher_t *cipher;
    h2_libsrtp_icm_state_t *state;
    const srtp_cipher_type_t *type;
    int algorithm;
    (void)tag_len;

    switch (key_len) {
    case SRTP_AES_ICM_128_KEY_LEN_WSALT:
        type = &srtp_aes_icm_128;
        algorithm = SRTP_AES_ICM_128;
        break;
    case SRTP_AES_ICM_192_KEY_LEN_WSALT:
        type = &srtp_aes_icm_192;
        algorithm = SRTP_AES_ICM_192;
        break;
    case SRTP_AES_ICM_256_KEY_LEN_WSALT:
        type = &srtp_aes_icm_256;
        algorithm = SRTP_AES_ICM_256;
        break;
    default:
        return srtp_err_status_bad_param;
    }
    cipher = srtp_crypto_alloc(sizeof(*cipher));
    state = srtp_crypto_alloc(sizeof(*state));
    if (cipher == NULL || state == NULL) {
        srtp_crypto_free(state);
        srtp_crypto_free(cipher);
        return srtp_err_status_alloc_fail;
    }
    cipher->type = type;
    cipher->state = state;
    cipher->key_len = key_len;
    cipher->algorithm = algorithm;
    state->key_len = (size_t)key_len - H2_LIBSRTP_AES_CM_SALT_SIZE;
    state->keystream_offset = sizeof(state->keystream);
    *out_cipher = cipher;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_icm_dealloc(srtp_cipher_t *cipher) {
    if (cipher == NULL) {
        return srtp_err_status_bad_param;
    }
    if (cipher->state != NULL) {
        h2_libsrtp_secure_zero(cipher->state, sizeof(h2_libsrtp_icm_state_t));
        srtp_crypto_free(cipher->state);
    }
    h2_libsrtp_secure_zero(cipher, sizeof(*cipher));
    srtp_crypto_free(cipher);
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_icm_init(
    void *raw_state, const uint8_t *key) {
    h2_libsrtp_icm_state_t *state = raw_state;
    memcpy(state->key, key, state->key_len);
    memset(state->offset, 0, sizeof(state->offset));
    memcpy(
        state->offset, key + state->key_len,
        H2_LIBSRTP_AES_CM_SALT_SIZE);
    memcpy(state->counter, state->offset, sizeof(state->counter));
    memset(state->keystream, 0, sizeof(state->keystream));
    state->keystream_offset = sizeof(state->keystream);
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_icm_set_iv(
    void *raw_state,
    uint8_t *iv,
    srtp_cipher_direction_t direction) {
    h2_libsrtp_icm_state_t *state = raw_state;
    size_t index;
    (void)direction;
    for (index = 0u; index < sizeof(state->counter); ++index) {
        state->counter[index] = state->offset[index] ^ iv[index];
    }
    state->keystream_offset = sizeof(state->keystream);
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_icm_crypt(
    void *raw_state, uint8_t *buffer, unsigned int *inout_len) {
    static const uint8_t zeros[H2_PAL_CRYPTO_AES_BLOCK_SIZE] = { 0 };
    h2_libsrtp_icm_state_t *state = raw_state;
    size_t remaining = (size_t)*inout_len;
    size_t available = state->keystream_offset < sizeof(state->keystream)
                           ? sizeof(state->keystream) - state->keystream_offset
                           : 0u;
    size_t new_blocks = remaining > available
                            ? (remaining - available + 15u) / 16u
                            : 0u;
    unsigned int low_counter =
        ((unsigned int)state->counter[14] << 8u) | state->counter[15];
    if (new_blocks > 0x10000u - low_counter) {
        return srtp_err_status_terminus;
    }
    while (remaining > 0u) {
        size_t chunk;
        size_t index;
        if (state->keystream_offset == sizeof(state->keystream)) {
            h2_pal_crypto_buf_t output = {
                .data = state->keystream,
                .len = 0u,
                .cap = sizeof(state->keystream),
            };
            h2_pal_result_t result = h2_pal_crypto_aes_ctr_xor(
                &h2_libsrtp_state.crypto, state->key, state->key_len,
                state->counter, zeros, sizeof(zeros), &output);
            if (result != H2_PAL_OK || output.len != sizeof(zeros)) {
                return h2_libsrtp_crypto_status(
                    result == H2_PAL_OK ? H2_PAL_ERR_IO : result,
                    srtp_err_status_cipher_fail);
            }
            h2_libsrtp_increment_counter(state->counter);
            state->keystream_offset = 0u;
        }
        chunk = sizeof(state->keystream) - state->keystream_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }
        for (index = 0u; index < chunk; ++index) {
            buffer[index] ^= state->keystream[state->keystream_offset + index];
        }
        buffer += chunk;
        remaining -= chunk;
        state->keystream_offset += chunk;
    }
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_gcm_alloc(
    srtp_cipher_t **out_cipher, int key_len, int tag_len) {
    srtp_cipher_t *cipher;
    h2_libsrtp_gcm_state_t *state;
    size_t allocation_size;
    const srtp_cipher_type_t *type;
    int algorithm;
    size_t base_key_len;
    if (tag_len != (int)H2_PAL_CRYPTO_AEAD_TAG_SIZE) {
        return srtp_err_status_bad_param;
    }
    if (key_len == SRTP_AES_GCM_128_KEY_LEN_WSALT) {
        type = &srtp_aes_gcm_128;
        algorithm = SRTP_AES_GCM_128;
        base_key_len = 16u;
    } else if (key_len == SRTP_AES_GCM_256_KEY_LEN_WSALT) {
        type = &srtp_aes_gcm_256;
        algorithm = SRTP_AES_GCM_256;
        base_key_len = 32u;
    } else {
        return srtp_err_status_bad_param;
    }
    if (h2_libsrtp_state.max_packet_size >
        SIZE_MAX - sizeof(*state)) {
        return srtp_err_status_bad_param;
    }
    allocation_size = sizeof(*state) + h2_libsrtp_state.max_packet_size;
    cipher = srtp_crypto_alloc(sizeof(*cipher));
    state = srtp_crypto_alloc(allocation_size);
    if (cipher == NULL || state == NULL) {
        srtp_crypto_free(state);
        srtp_crypto_free(cipher);
        return srtp_err_status_alloc_fail;
    }
    cipher->type = type;
    cipher->state = state;
    cipher->key_len = key_len;
    cipher->algorithm = algorithm;
    state->key_len = base_key_len;
    state->tag_len = (size_t)tag_len;
    state->direction = srtp_direction_any;
    *out_cipher = cipher;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_gcm_dealloc(srtp_cipher_t *cipher) {
    h2_libsrtp_gcm_state_t *state;
    if (cipher == NULL) {
        return srtp_err_status_bad_param;
    }
    state = cipher->state;
    if (state != NULL) {
        h2_libsrtp_secure_zero(
            state, sizeof(*state) + h2_libsrtp_state.max_packet_size);
        srtp_crypto_free(state);
    }
    h2_libsrtp_secure_zero(cipher, sizeof(*cipher));
    srtp_crypto_free(cipher);
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_gcm_init(
    void *raw_state, const uint8_t *key) {
    h2_libsrtp_gcm_state_t *state = raw_state;
    memcpy(state->key, key, state->key_len);
    state->aad_len = 0u;
    state->direction = srtp_direction_any;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_gcm_set_iv(
    void *raw_state,
    uint8_t *iv,
    srtp_cipher_direction_t direction) {
    h2_libsrtp_gcm_state_t *state = raw_state;
    if (direction != srtp_direction_encrypt &&
        direction != srtp_direction_decrypt) {
        return srtp_err_status_bad_param;
    }
    memcpy(state->iv, iv, sizeof(state->iv));
    state->direction = direction;
    state->aad_len = 0u;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_gcm_set_aad(
    void *raw_state, const uint8_t *aad, uint32_t aad_len) {
    h2_libsrtp_gcm_state_t *state = raw_state;
    if ((aad == NULL && aad_len != 0u) ||
        (size_t)aad_len > h2_libsrtp_state.max_packet_size - state->aad_len) {
        return srtp_err_status_bad_param;
    }
    if (aad_len != 0u) {
        memcpy(state->aad + state->aad_len, aad, aad_len);
    }
    state->aad_len += aad_len;
    return srtp_err_status_ok;
}

static h2_pal_crypto_aead_algorithm_t h2_libsrtp_gcm_algorithm(
    const h2_libsrtp_gcm_state_t *state) {
    return state->key_len == 16u ? H2_PAL_CRYPTO_AEAD_AES_128_GCM
                                 : H2_PAL_CRYPTO_AEAD_AES_256_GCM;
}

static srtp_err_status_t h2_libsrtp_gcm_encrypt(
    void *raw_state, uint8_t *buffer, unsigned int *inout_len) {
    h2_libsrtp_gcm_state_t *state = raw_state;
    uint8_t empty_output[H2_PAL_CRYPTO_AEAD_TAG_SIZE];
    h2_pal_crypto_buf_t output = {
        .data = buffer != NULL ? buffer : empty_output,
        .len = 0u,
        .cap = (size_t)*inout_len + H2_PAL_CRYPTO_AEAD_TAG_SIZE,
    };
    h2_pal_result_t result;
    if (state->direction != srtp_direction_encrypt ||
        (buffer == NULL && *inout_len != 0u) ||
        (size_t)*inout_len > h2_libsrtp_state.max_packet_size -
                                 H2_PAL_CRYPTO_AEAD_TAG_SIZE) {
        return srtp_err_status_bad_param;
    }
    result = h2_pal_crypto_aead_seal(
        &h2_libsrtp_state.crypto, h2_libsrtp_gcm_algorithm(state),
        state->key, state->key_len, state->iv, sizeof(state->iv),
        buffer, *inout_len, state->aad, state->aad_len, &output);
    state->aad_len = 0u;
    if (result != H2_PAL_OK ||
        output.len != (size_t)*inout_len + state->tag_len) {
        return h2_libsrtp_crypto_status(
            result == H2_PAL_OK ? H2_PAL_ERR_IO : result,
            srtp_err_status_cipher_fail);
    }
    memcpy(
        state->tag,
        output.data + output.len - state->tag_len,
        state->tag_len);
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_gcm_get_tag(
    void *raw_state, uint8_t *tag, uint32_t *tag_len) {
    h2_libsrtp_gcm_state_t *state = raw_state;
    memcpy(tag, state->tag, state->tag_len);
    *tag_len = (uint32_t)state->tag_len;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_gcm_decrypt(
    void *raw_state, uint8_t *buffer, unsigned int *inout_len) {
    h2_libsrtp_gcm_state_t *state = raw_state;
    h2_pal_crypto_buf_t output;
    h2_pal_result_t result;
    if (state->direction != srtp_direction_decrypt || buffer == NULL ||
        *inout_len < state->tag_len) {
        return srtp_err_status_bad_param;
    }
    output.data = buffer;
    output.len = 0u;
    output.cap = (size_t)*inout_len - state->tag_len;
    result = h2_pal_crypto_aead_open(
        &h2_libsrtp_state.crypto, h2_libsrtp_gcm_algorithm(state),
        state->key, state->key_len, state->iv, sizeof(state->iv),
        buffer, *inout_len, state->aad, state->aad_len, &output);
    state->aad_len = 0u;
    if (result != H2_PAL_OK) {
        return h2_libsrtp_crypto_status(result, srtp_err_status_auth_fail);
    }
    if (output.len != (size_t)*inout_len - state->tag_len) {
        return h2_libsrtp_crypto_status(
            H2_PAL_ERR_IO, srtp_err_status_cipher_fail);
    }
    *inout_len = (unsigned int)output.len;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_hmac_alloc(
    srtp_auth_t **out_auth, int key_len, int out_len) {
    uint8_t *allocation;
    srtp_auth_t *auth;
    h2_libsrtp_hmac_state_t *state;
    size_t allocation_size;
    if (key_len < 0 || key_len > (int)H2_PAL_CRYPTO_SHA1_SIZE ||
        out_len < 0 || out_len > (int)H2_PAL_CRYPTO_SHA1_SIZE ||
        h2_libsrtp_state.max_packet_size >
            SIZE_MAX - sizeof(*auth) - sizeof(*state)) {
        return srtp_err_status_bad_param;
    }
    allocation_size = sizeof(*auth) + sizeof(*state) +
                      h2_libsrtp_state.max_packet_size;
    allocation = srtp_crypto_alloc(allocation_size);
    if (allocation == NULL) {
        return srtp_err_status_alloc_fail;
    }
    auth = (srtp_auth_t *)allocation;
    state = (h2_libsrtp_hmac_state_t *)(allocation + sizeof(*auth));
    auth->type = &srtp_hmac;
    auth->state = state;
    auth->out_len = out_len;
    auth->key_len = key_len;
    auth->prefix_len = 0;
    state->capacity = h2_libsrtp_state.max_packet_size;
    *out_auth = auth;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_hmac_dealloc(srtp_auth_t *auth) {
    h2_libsrtp_hmac_state_t *state;
    size_t allocation_size;
    if (auth == NULL || auth->state == NULL) {
        return srtp_err_status_bad_param;
    }
    state = auth->state;
    allocation_size = sizeof(*auth) + sizeof(*state) + state->capacity;
    h2_libsrtp_secure_zero(auth, allocation_size);
    srtp_crypto_free(auth);
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_hmac_init(
    void *raw_state, const uint8_t *key, int key_len) {
    h2_libsrtp_hmac_state_t *state = raw_state;
    if (key_len < 0 || (key == NULL && key_len != 0) ||
        key_len > (int)sizeof(state->key)) {
        return srtp_err_status_bad_param;
    }
    if (key_len != 0) {
        memcpy(state->key, key, (size_t)key_len);
    }
    state->key_len = (size_t)key_len;
    state->data_len = 0u;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_hmac_start(void *raw_state) {
    h2_libsrtp_hmac_state_t *state = raw_state;
    state->data_len = 0u;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_hmac_update(
    void *raw_state, const uint8_t *data, int data_len) {
    h2_libsrtp_hmac_state_t *state = raw_state;
    if (data_len < 0 || (data == NULL && data_len != 0) ||
        (size_t)data_len > state->capacity - state->data_len) {
        return srtp_err_status_bad_param;
    }
    if (data_len != 0) {
        memcpy(state->data + state->data_len, data, (size_t)data_len);
    }
    state->data_len += (size_t)data_len;
    return srtp_err_status_ok;
}

static srtp_err_status_t h2_libsrtp_hmac_compute(
    void *raw_state,
    const uint8_t *data,
    int data_len,
    int tag_len,
    uint8_t *tag) {
    h2_libsrtp_hmac_state_t *state = raw_state;
    uint8_t digest[H2_PAL_CRYPTO_SHA1_SIZE];
    h2_pal_result_t result;
    srtp_err_status_t status = h2_libsrtp_hmac_update(
        state, data, data_len);
    if (status != srtp_err_status_ok || tag_len < 0 ||
        tag_len > (int)sizeof(digest)) {
        return srtp_err_status_bad_param;
    }
    result = h2_pal_crypto_hmac_sha1(
        &h2_libsrtp_state.crypto, state->key, state->key_len,
        state->data, state->data_len, digest);
    if (result != H2_PAL_OK) {
        h2_libsrtp_secure_zero(digest, sizeof(digest));
        return h2_libsrtp_crypto_status(
            result, srtp_err_status_auth_fail);
    }
    memcpy(tag, digest, (size_t)tag_len);
    h2_libsrtp_secure_zero(digest, sizeof(digest));
    return srtp_err_status_ok;
}

static uint8_t h2_gcm_zero_iv[H2_PAL_CRYPTO_AEAD_NONCE_SIZE];
static const uint8_t h2_gcm_128_zero_key[
    SRTP_AES_GCM_128_KEY_LEN_WSALT] = {0};
static const uint8_t h2_gcm_256_zero_key[
    SRTP_AES_GCM_256_KEY_LEN_WSALT] = {0};
static const uint8_t h2_gcm_128_empty_tag[16] = {
    0x58, 0xe2, 0xfc, 0xce, 0xfa, 0x7e, 0x30, 0x61,
    0x36, 0x7f, 0x1d, 0x57, 0xa4, 0xe7, 0x45, 0x5a,
};
static const uint8_t h2_gcm_256_empty_tag[16] = {
    0x53, 0x0f, 0x8a, 0xfb, 0xc7, 0x45, 0x36, 0xb9,
    0xa9, 0x63, 0xb4, 0xf1, 0xc4, 0xcb, 0x73, 0x8b,
};
static const srtp_cipher_test_case_t h2_gcm_128_test = {
    SRTP_AES_GCM_128_KEY_LEN_WSALT, h2_gcm_128_zero_key,
    h2_gcm_zero_iv, 0u, NULL, 16u, h2_gcm_128_empty_tag,
    0, NULL, 16, NULL,
};
static const srtp_cipher_test_case_t h2_gcm_256_test = {
    SRTP_AES_GCM_256_KEY_LEN_WSALT, h2_gcm_256_zero_key,
    h2_gcm_zero_iv, 0u, NULL, 16u, h2_gcm_256_empty_tag,
    0, NULL, 16, NULL,
};

const srtp_cipher_type_t srtp_aes_icm_128 = {
    h2_libsrtp_icm_alloc, h2_libsrtp_icm_dealloc, h2_libsrtp_icm_init,
    NULL, h2_libsrtp_icm_crypt, h2_libsrtp_icm_crypt,
    h2_libsrtp_icm_set_iv, NULL, "AES-128 ICM through H2 Crypto PAL",
    &srtp_aes_icm_128_test_case_0, SRTP_AES_ICM_128,
};

const srtp_cipher_type_t srtp_aes_icm_192 = {
    h2_libsrtp_icm_alloc, h2_libsrtp_icm_dealloc, h2_libsrtp_icm_init,
    NULL, h2_libsrtp_icm_crypt, h2_libsrtp_icm_crypt,
    h2_libsrtp_icm_set_iv, NULL, "AES-192 ICM through H2 Crypto PAL",
    &srtp_aes_icm_192_test_case_0, SRTP_AES_ICM_192,
};

const srtp_cipher_type_t srtp_aes_icm_256 = {
    h2_libsrtp_icm_alloc, h2_libsrtp_icm_dealloc, h2_libsrtp_icm_init,
    NULL, h2_libsrtp_icm_crypt, h2_libsrtp_icm_crypt,
    h2_libsrtp_icm_set_iv, NULL, "AES-256 ICM through H2 Crypto PAL",
    &srtp_aes_icm_256_test_case_0, SRTP_AES_ICM_256,
};

const srtp_cipher_type_t srtp_aes_gcm_128 = {
    h2_libsrtp_gcm_alloc, h2_libsrtp_gcm_dealloc, h2_libsrtp_gcm_init,
    h2_libsrtp_gcm_set_aad, h2_libsrtp_gcm_encrypt,
    h2_libsrtp_gcm_decrypt, h2_libsrtp_gcm_set_iv,
    h2_libsrtp_gcm_get_tag, "AES-128 GCM through H2 Crypto PAL",
    &h2_gcm_128_test, SRTP_AES_GCM_128,
};

const srtp_cipher_type_t srtp_aes_gcm_256 = {
    h2_libsrtp_gcm_alloc, h2_libsrtp_gcm_dealloc, h2_libsrtp_gcm_init,
    h2_libsrtp_gcm_set_aad, h2_libsrtp_gcm_encrypt,
    h2_libsrtp_gcm_decrypt, h2_libsrtp_gcm_set_iv,
    h2_libsrtp_gcm_get_tag, "AES-256 GCM through H2 Crypto PAL",
    &h2_gcm_256_test, SRTP_AES_GCM_256,
};

const srtp_auth_type_t srtp_hmac = {
    h2_libsrtp_hmac_alloc, h2_libsrtp_hmac_dealloc,
    h2_libsrtp_hmac_init, h2_libsrtp_hmac_compute,
    h2_libsrtp_hmac_update, h2_libsrtp_hmac_start,
    "HMAC-SHA1 through H2 Crypto PAL", &srtp_hmac_test_case_0,
    SRTP_HMAC_SHA1,
};
