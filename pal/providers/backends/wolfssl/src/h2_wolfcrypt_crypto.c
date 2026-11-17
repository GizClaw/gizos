#include "h2_wolfcrypt_crypto_internal.h"

#include "h2_pal_unsupported.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/chacha20_poly1305.h>
#include <wolfssl/wolfcrypt/curve25519.h>
#include <wolfssl/wolfcrypt/des3.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/integer.h>
#include <wolfssl/wolfcrypt/md5.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>
#ifdef WOLFSSL_SM2
#include <wolfssl/wolfcrypt/sm2.h>
#endif

#define H2_WOLFCRYPT_AEAD_NONCE_SIZE 12u
#define H2_WOLFCRYPT_AEAD_TAG_SIZE 16u
#define H2_WOLFCRYPT_SHA256_SIZE 32u
#define H2_WOLFCRYPT_HKDF_MAX_SIZE (255u * H2_WOLFCRYPT_SHA256_SIZE)

typedef struct h2_wolfcrypt_state {
  void* entropy_user;
  h2_wolfcrypt_entropy_fn entropy;
  int ready;
} h2_wolfcrypt_state_t;

static h2_wolfcrypt_state_t h2_wolfcrypt_state;

void h2_wolfcrypt_secure_zero(void* data, size_t len) {
  volatile uint8_t* bytes = (volatile uint8_t*)data;
  while (len > 0u) {
    *bytes++ = 0u;
    --len;
  }
}

int h2_wolfcrypt_entropy_fill(uint8_t* out, size_t len) {
  if (!h2_wolfcrypt_state.ready || h2_wolfcrypt_state.entropy == NULL) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (out == NULL && len != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_wolfcrypt_state.entropy(h2_wolfcrypt_state.entropy_user, out, len);
}

static h2_pal_result_t h2_wolfcrypt_random(void* user, uint8_t* out,
                                           size_t len) {
  int rc;
  (void)user;
  if ((out == NULL && len != 0u) || len > (size_t)INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = h2_wolfcrypt_entropy_fill(out, len);
  if (rc != H2_PAL_OK) {
    h2_wolfcrypt_secure_zero(out, len);
  }
  return rc;
}

static int h2_size_to_word32(size_t value, word32* out) {
  if (value > (size_t)INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out = (word32)value;
  return H2_PAL_OK;
}

static int h2_all_zero(const uint8_t* data, size_t len) {
  uint8_t value = 0u;
  size_t index;
  for (index = 0u; index < len; ++index) {
    value |= data[index];
  }
  return value == 0u;
}

static int h2_p256_scalar_valid(const uint8_t* scalar) {
  static const uint8_t order[H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE] = {
      0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
      0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
  };
  if (h2_all_zero(scalar, H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE)) {
    return 0;
  }
  return memcmp(scalar, order, sizeof(order)) < 0;
}

static int h2_p256_private_key_valid(
    const h2_pal_p256_private_key_t* private_key) {
  return h2_p256_scalar_valid(private_key->bytes);
}

static int h2_wolfcrypt_ecc_make_public(ecc_key* key, WC_RNG* rng) {
  int (*make_public)(ecc_key*, ecc_point*, WC_RNG*) = wc_ecc_make_pub_ex;
  return make_public(key, NULL, rng);
}

static h2_pal_result_t h2_wolfcrypt_x25519_public_key_from_private(
    void* user, const h2_pal_x25519_private_key_t* private_key,
    h2_pal_x25519_public_key_t* out_public_key) {
  curve25519_key key;
  WC_RNG rng;
  uint8_t private_copy[H2_PAL_CRYPTO_X25519_KEY_SIZE];
  word32 public_len = H2_PAL_CRYPTO_X25519_KEY_SIZE;
  int key_ready = 0;
  int rng_ready = 0;
  int wolf_rc;
  (void)user;

  if (private_key == NULL || out_public_key == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_public_key, 0, sizeof(*out_public_key));
  memcpy(private_copy, private_key->bytes, sizeof(private_copy));
  private_copy[0] &= 248u;
  private_copy[31] &= 127u;
  private_copy[31] |= 64u;

  wolf_rc = wc_curve25519_init(&key);
  if (wolf_rc == 0) {
    key_ready = 1;
    wolf_rc = wc_InitRng(&rng);
  }
  if (wolf_rc == 0) {
    rng_ready = 1;
    wolf_rc = wc_curve25519_import_private_ex(private_copy,
                                              H2_PAL_CRYPTO_X25519_KEY_SIZE,
                                              &key, EC25519_LITTLE_ENDIAN);
  }
#ifdef WOLFSSL_CURVE25519_BLINDING
  if (wolf_rc == 0) {
    wolf_rc = wc_curve25519_set_rng(&key, &rng);
  }
#endif
  if (wolf_rc == 0) {
    wolf_rc = wc_curve25519_export_public_ex(
        &key, out_public_key->bytes, &public_len, EC25519_LITTLE_ENDIAN);
  }
  if (rng_ready) {
    (void)wc_FreeRng(&rng);
  }
  if (key_ready) {
    wc_curve25519_free(&key);
  }
  h2_wolfcrypt_secure_zero(private_copy, sizeof(private_copy));
  if (wolf_rc != 0 || public_len != H2_PAL_CRYPTO_X25519_KEY_SIZE) {
    h2_wolfcrypt_secure_zero(out_public_key, sizeof(*out_public_key));
    return H2_PAL_ERR_IO;
  }
  out_public_key->bytes[31] &= 0x7fu;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_x25519_keypair_generate(
    void* user, h2_pal_x25519_keypair_t* out_keypair) {
  int rc;
  if (out_keypair == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_keypair, 0, sizeof(*out_keypair));
  rc = h2_wolfcrypt_random(user, out_keypair->private_key.bytes,
                           sizeof(out_keypair->private_key.bytes));
  if (rc == H2_PAL_OK) {
    rc = h2_wolfcrypt_x25519_public_key_from_private(
        user, &out_keypair->private_key, &out_keypair->public_key);
  }
  if (rc != H2_PAL_OK) {
    h2_wolfcrypt_secure_zero(out_keypair, sizeof(*out_keypair));
  }
  return rc;
}

static h2_pal_result_t h2_wolfcrypt_x25519_shared_secret(
    void* user, const h2_pal_x25519_private_key_t* local_private_key,
    const h2_pal_x25519_public_key_t* remote_public_key,
    h2_pal_x25519_shared_secret_t* out_shared_secret) {
  curve25519_key private_key;
  curve25519_key public_key;
  WC_RNG rng;
  uint8_t private_copy[H2_PAL_CRYPTO_X25519_KEY_SIZE];
  uint8_t remote_copy[H2_PAL_CRYPTO_X25519_KEY_SIZE];
  word32 shared_len = H2_PAL_CRYPTO_X25519_KEY_SIZE;
  int private_ready = 0;
  int public_ready = 0;
  int rng_ready = 0;
  int wolf_rc;
  int rc = H2_PAL_OK;
  (void)user;

  if (local_private_key == NULL || remote_public_key == NULL ||
      out_shared_secret == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_shared_secret, 0, sizeof(*out_shared_secret));
  memcpy(private_copy, local_private_key->bytes, sizeof(private_copy));
  private_copy[0] &= 248u;
  private_copy[31] &= 127u;
  private_copy[31] |= 64u;
  memcpy(remote_copy, remote_public_key->bytes, sizeof(remote_copy));
  remote_copy[31] &= 0x7fu;
  if (h2_all_zero(remote_copy, sizeof(remote_copy))) {
    h2_wolfcrypt_secure_zero(private_copy, sizeof(private_copy));
    return H2_PAL_ERR_FORMAT;
  }
  wolf_rc = wc_curve25519_init(&private_key);
  if (wolf_rc == 0) {
    private_ready = 1;
    wolf_rc = wc_curve25519_init(&public_key);
  }
  if (wolf_rc == 0) {
    public_ready = 1;
    wolf_rc = wc_InitRng(&rng);
  }
  if (wolf_rc == 0) {
    rng_ready = 1;
    wolf_rc = wc_curve25519_import_private_ex(
        private_copy, H2_PAL_CRYPTO_X25519_KEY_SIZE, &private_key,
        EC25519_LITTLE_ENDIAN);
  }
  if (wolf_rc == 0) {
    wolf_rc = wc_curve25519_import_public_ex(
        remote_copy, H2_PAL_CRYPTO_X25519_KEY_SIZE, &public_key,
        EC25519_LITTLE_ENDIAN);
    if (wolf_rc != 0) {
      rc = H2_PAL_ERR_FORMAT;
    }
  }
#ifdef WOLFSSL_CURVE25519_BLINDING
  if (wolf_rc == 0) {
    wolf_rc = wc_curve25519_set_rng(&private_key, &rng);
  }
#endif
  if (wolf_rc == 0) {
    wolf_rc = wc_curve25519_shared_secret_ex(
        &private_key, &public_key, out_shared_secret->bytes, &shared_len,
        EC25519_LITTLE_ENDIAN);
  }
  if (rng_ready) {
    (void)wc_FreeRng(&rng);
  }
  if (public_ready) {
    wc_curve25519_free(&public_key);
  }
  if (private_ready) {
    wc_curve25519_free(&private_key);
  }
  h2_wolfcrypt_secure_zero(private_copy, sizeof(private_copy));
  h2_wolfcrypt_secure_zero(remote_copy, sizeof(remote_copy));
  if (wolf_rc != 0 || shared_len != H2_PAL_CRYPTO_X25519_KEY_SIZE) {
    h2_wolfcrypt_secure_zero(out_shared_secret, sizeof(*out_shared_secret));
    return rc == H2_PAL_ERR_FORMAT ? rc : H2_PAL_ERR_IO;
  }
  if (h2_all_zero(out_shared_secret->bytes, sizeof(out_shared_secret->bytes))) {
    h2_wolfcrypt_secure_zero(out_shared_secret, sizeof(*out_shared_secret));
    return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_hkdf_sha256(
    void* user, const uint8_t* secret, size_t secret_len, const uint8_t* salt,
    size_t salt_len, const uint8_t* info, size_t info_len, uint8_t* out,
    size_t out_len) {
  static uint8_t empty = 0u;
  word32 secret_size;
  word32 salt_size;
  word32 info_size;
  word32 output_size;
  int wolf_rc;
  (void)user;

  if ((secret == NULL && secret_len != 0u) ||
      (salt == NULL && salt_len != 0u) || (info == NULL && info_len != 0u) ||
      (out == NULL && out_len != 0u) || out_len > H2_WOLFCRYPT_HKDF_MAX_SIZE ||
      h2_size_to_word32(secret_len, &secret_size) != H2_PAL_OK ||
      h2_size_to_word32(salt_len, &salt_size) != H2_PAL_OK ||
      h2_size_to_word32(info_len, &info_size) != H2_PAL_OK ||
      h2_size_to_word32(out_len, &output_size) != H2_PAL_OK) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  wolf_rc = wc_HKDF(WC_SHA256, secret_len == 0u ? &empty : secret, secret_size,
                    salt_len == 0u ? NULL : salt, salt_size,
                    info_len == 0u ? &empty : info, info_size,
                    out_len == 0u ? &empty : out, output_size);
  if (wolf_rc != 0) {
    h2_wolfcrypt_secure_zero(out, out_len);
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static int h2_wolfcrypt_nonexact_overlap(const uint8_t* input, size_t input_len,
                                         uint8_t* output, size_t output_len);

static int h2_wolfcrypt_aead(h2_pal_crypto_aead_algorithm_t algorithm,
                             const uint8_t* key, size_t key_len,
                             const uint8_t* nonce, size_t nonce_len,
                             const uint8_t* input, size_t input_len,
                             const uint8_t* aad, size_t aad_len,
                             h2_pal_crypto_buf_t* out, int seal) {
  static const uint8_t empty = 0u;
  word32 input_size;
  word32 aad_size;
  size_t body_len;
  size_t needed;
  uint8_t empty_output = 0u;
  int wolf_rc;

  if (out == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out->len = 0u;
  if ((key == NULL && key_len != 0u) || nonce == NULL ||
      nonce_len != H2_WOLFCRYPT_AEAD_NONCE_SIZE ||
      (input == NULL && input_len != 0u) || (aad == NULL && aad_len != 0u) ||
      (out->data == NULL && out->cap != 0u) ||
      h2_size_to_word32(aad_len, &aad_size) != H2_PAL_OK) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (algorithm != H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305 &&
      algorithm != H2_PAL_CRYPTO_AEAD_AES_128_GCM &&
      algorithm != H2_PAL_CRYPTO_AEAD_AES_256_GCM) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  if (input_len > (size_t)INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if ((algorithm == H2_PAL_CRYPTO_AEAD_AES_128_GCM && key_len != 16u) ||
      (algorithm != H2_PAL_CRYPTO_AEAD_AES_128_GCM && key_len != 32u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!seal && input_len < H2_WOLFCRYPT_AEAD_TAG_SIZE) {
    return H2_PAL_ERR_FORMAT;
  }
  if (seal && input_len > (size_t)(INT_MAX - H2_WOLFCRYPT_AEAD_TAG_SIZE)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  body_len = seal ? input_len : input_len - H2_WOLFCRYPT_AEAD_TAG_SIZE;
  needed = seal ? input_len + H2_WOLFCRYPT_AEAD_TAG_SIZE : body_len;
  if (needed < body_len ||
      h2_size_to_word32(body_len, &input_size) != H2_PAL_OK) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (out->cap < needed) {
    return H2_PAL_ERR_NO_SPACE;
  }
  if (needed != 0u && out->data == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (h2_wolfcrypt_nonexact_overlap(input, input_len, out->data, needed)) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  if (algorithm == H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305) {
    if (seal) {
      wolf_rc = wc_ChaCha20Poly1305_Encrypt(
          key, nonce, aad_len == 0u ? &empty : aad, aad_size,
          input_len == 0u ? &empty : input, input_size, out->data,
          out->data + body_len);
    } else {
      wolf_rc = wc_ChaCha20Poly1305_Decrypt(
          key, nonce, aad_len == 0u ? &empty : aad, aad_size,
          body_len == 0u ? &empty : input, input_size, input + body_len,
          body_len == 0u ? &empty_output : out->data);
    }
  } else {
    Aes aes;
    int aes_ready = 0;
    wolf_rc = wc_AesInit(&aes, NULL, INVALID_DEVID);
    if (wolf_rc == 0) {
      aes_ready = 1;
      wolf_rc = wc_AesGcmSetKey(&aes, key, (word32)key_len);
    }
    if (wolf_rc == 0 && seal) {
      wolf_rc = wc_AesGcmEncrypt(
          &aes, out->data, input_len == 0u ? &empty : input, input_size, nonce,
          H2_WOLFCRYPT_AEAD_NONCE_SIZE, out->data + body_len,
          H2_WOLFCRYPT_AEAD_TAG_SIZE, aad_len == 0u ? &empty : aad, aad_size);
    } else if (wolf_rc == 0) {
      wolf_rc = wc_AesGcmDecrypt(
          &aes, body_len == 0u ? &empty_output : out->data,
          body_len == 0u ? &empty : input, input_size, nonce,
          H2_WOLFCRYPT_AEAD_NONCE_SIZE, input + body_len,
          H2_WOLFCRYPT_AEAD_TAG_SIZE, aad_len == 0u ? &empty : aad, aad_size);
    }
    if (aes_ready) {
      wc_AesFree(&aes);
    }
  }
  if (wolf_rc != 0) {
    h2_wolfcrypt_secure_zero(out->data, needed);
    if (!seal && (wolf_rc == AES_GCM_AUTH_E || wolf_rc == MAC_CMP_FAILED_E)) {
      return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_ERR_IO;
  }
  out->len = needed;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_aead_seal(
    void* user, h2_pal_crypto_aead_algorithm_t algorithm, const uint8_t* key,
    size_t key_len, const uint8_t* nonce, size_t nonce_len,
    const uint8_t* plaintext, size_t plaintext_len, const uint8_t* aad,
    size_t aad_len, h2_pal_crypto_buf_t* out_ciphertext) {
  (void)user;
  return h2_wolfcrypt_aead(algorithm, key, key_len, nonce, nonce_len, plaintext,
                           plaintext_len, aad, aad_len, out_ciphertext, 1);
}

static h2_pal_result_t h2_wolfcrypt_aead_open(
    void* user, h2_pal_crypto_aead_algorithm_t algorithm, const uint8_t* key,
    size_t key_len, const uint8_t* nonce, size_t nonce_len,
    const uint8_t* ciphertext, size_t ciphertext_len, const uint8_t* aad,
    size_t aad_len, h2_pal_crypto_buf_t* out_plaintext) {
  (void)user;
  return h2_wolfcrypt_aead(algorithm, key, key_len, nonce, nonce_len,
                           ciphertext, ciphertext_len, aad, aad_len,
                           out_plaintext, 0);
}

static int h2_wolfcrypt_counter_has_capacity(
    const uint8_t counter[H2_PAL_CRYPTO_AES_BLOCK_SIZE], size_t input_len) {
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
  for (index = H2_PAL_CRYPTO_AES_BLOCK_SIZE; index > 0u && increment != 0u;
       --index) {
    unsigned int sum =
        (unsigned int)counter[index - 1u] + (unsigned int)(increment & 0xffu);
    increment >>= 8u;
    if (sum > 0xffu) {
      ++increment;
    }
  }
  return increment == 0u;
}

static int h2_wolfcrypt_nonexact_overlap(const uint8_t* input, size_t input_len,
                                         uint8_t* output, size_t output_len) {
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

static h2_pal_result_t h2_wolfcrypt_aes_ctr_xor(
    void* user, const uint8_t* key, size_t key_len,
    const uint8_t initial_counter[H2_PAL_CRYPTO_AES_BLOCK_SIZE],
    const uint8_t* input, size_t input_len, h2_pal_crypto_buf_t* out) {
  Aes aes;
  int aes_ready = 0;
  int wolf_rc;
  (void)user;
  if (out == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out->len = 0u;
  if (key == NULL || initial_counter == NULL ||
      (input == NULL && input_len != 0u) ||
      (out->data == NULL && out->cap != 0u) ||
      (key_len != 16u && key_len != 24u && key_len != 32u) ||
      input_len > (size_t)UINT32_MAX ||
      !h2_wolfcrypt_counter_has_capacity(initial_counter, input_len) ||
      h2_wolfcrypt_nonexact_overlap(input, input_len, out->data, input_len)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (out->cap < input_len) {
    return H2_PAL_ERR_NO_SPACE;
  }
  if (input_len == 0u) {
    return H2_PAL_OK;
  }
  wolf_rc = wc_AesInit(&aes, NULL, INVALID_DEVID);
  if (wolf_rc == 0) {
    aes_ready = 1;
    wolf_rc = wc_AesSetKey(&aes, key, (word32)key_len, initial_counter,
                           AES_ENCRYPTION);
  }
  if (wolf_rc == 0) {
    wolf_rc = wc_AesCtrEncrypt(&aes, out->data, input, (word32)input_len);
  }
  if (aes_ready) {
    wc_AesFree(&aes);
  }
  if (wolf_rc != 0) {
    h2_wolfcrypt_secure_zero(out->data, input_len);
    return H2_PAL_ERR_IO;
  }
  out->len = input_len;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_aes_cbc(
    void* user, h2_pal_crypto_cipher_direction_t direction, const uint8_t* key,
    size_t key_len, const uint8_t iv[16], const uint8_t* input,
    size_t input_len, h2_pal_crypto_buf_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  out->len = 0u;
  if (key == NULL || iv == NULL ||
      (key_len != 16u && key_len != 24u && key_len != 32u) ||
      (direction != H2_PAL_CRYPTO_CIPHER_ENCRYPT &&
       direction != H2_PAL_CRYPTO_CIPHER_DECRYPT) ||
      (input == NULL && input_len != 0u) || input_len % 16u != 0u ||
      input_len > UINT32_MAX || (out->data == NULL && out->cap != 0u) ||
      h2_wolfcrypt_nonexact_overlap(input, input_len, out->data, input_len))
    return H2_PAL_ERR_INVALID_ARG;
  if (out->cap < input_len) return H2_PAL_ERR_NO_SPACE;
  if (input_len == 0u) return H2_PAL_OK;
  Aes aes;
  int rc = wc_AesInit(&aes, NULL, INVALID_DEVID);
  if (rc != 0) return H2_PAL_ERR_IO;
  rc = wc_AesSetKey(&aes, key, (word32)key_len, iv,
                    direction == H2_PAL_CRYPTO_CIPHER_ENCRYPT ? AES_ENCRYPTION
                                                              : AES_DECRYPTION);
  if (rc == 0)
    rc = direction == H2_PAL_CRYPTO_CIPHER_ENCRYPT
             ? wc_AesCbcEncrypt(&aes, out->data, input, (word32)input_len)
             : wc_AesCbcDecrypt(&aes, out->data, input, (word32)input_len);
  wc_AesFree(&aes);
  h2_wolfcrypt_secure_zero(&aes, sizeof(aes));
  if (rc != 0) {
    h2_wolfcrypt_secure_zero(out->data, input_len);
    return H2_PAL_ERR_IO;
  }
  out->len = input_len;
  return H2_PAL_OK;
}

#ifndef NO_DES3
static h2_pal_result_t h2_wolfcrypt_des_cbc(
    void* user, h2_pal_crypto_cipher_direction_t direction, const uint8_t* key,
    size_t key_length, const uint8_t iv[8], const uint8_t* input, size_t length,
    h2_pal_crypto_buf_t* output) {
  (void)user;
  if (output == NULL) return H2_PAL_ERR_INVALID_ARG;
  output->len = 0u;
  if (key == NULL || iv == NULL ||
      (key_length != 8u && key_length != 16u && key_length != 24u) ||
      (direction != H2_PAL_CRYPTO_CIPHER_ENCRYPT &&
       direction != H2_PAL_CRYPTO_CIPHER_DECRYPT) ||
      (input == NULL && length != 0u) || length % 8u != 0u ||
      length > UINT32_MAX || (output->data == NULL && output->cap != 0u) ||
      h2_wolfcrypt_nonexact_overlap(input, length, output->data, length))
    return H2_PAL_ERR_INVALID_ARG;
  if (output->cap < length) return H2_PAL_ERR_NO_SPACE;
  if (length == 0u) return H2_PAL_OK;
  int rc;
  int mode = direction == H2_PAL_CRYPTO_CIPHER_ENCRYPT ? DES_ENCRYPTION
                                                       : DES_DECRYPTION;
  if (key_length == 8u) {
    Des des;
    memset(&des, 0, sizeof(des));
    rc = wc_Des_SetKey(&des, key, iv, mode);
    if (rc == 0)
      rc = direction == H2_PAL_CRYPTO_CIPHER_ENCRYPT
               ? wc_Des_CbcEncrypt(&des, output->data, input, (word32)length)
               : wc_Des_CbcDecrypt(&des, output->data, input, (word32)length);
    h2_wolfcrypt_secure_zero(&des, sizeof(des));
  } else {
    uint8_t expanded[24];
    memcpy(expanded, key, key_length);
    if (key_length == 16u) memcpy(expanded + 16u, key, 8u);
    Des3 des;
    rc = wc_Des3Init(&des, NULL, INVALID_DEVID);
    if (rc == 0) {
      rc = wc_Des3_SetKey(&des, expanded, iv, mode);
      if (rc == 0)
        rc =
            direction == H2_PAL_CRYPTO_CIPHER_ENCRYPT
                ? wc_Des3_CbcEncrypt(&des, output->data, input, (word32)length)
                : wc_Des3_CbcDecrypt(&des, output->data, input, (word32)length);
      wc_Des3Free(&des);
    }
    h2_wolfcrypt_secure_zero(&des, sizeof(des));
    h2_wolfcrypt_secure_zero(expanded, sizeof(expanded));
  }
  if (rc != 0) {
    h2_wolfcrypt_secure_zero(output->data, length);
    return H2_PAL_ERR_IO;
  }
  output->len = length;
  return H2_PAL_OK;
}
#endif

static h2_pal_result_t h2_wolfcrypt_md5(
    void* user, const uint8_t* input, size_t input_len,
    uint8_t out_digest[H2_PAL_CRYPTO_MD5_SIZE]) {
  word32 length;
  int rc;
  (void)user;
  if ((input == NULL && input_len != 0u) || out_digest == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_digest, 0, H2_PAL_CRYPTO_MD5_SIZE);
  rc = h2_size_to_word32(input_len, &length);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (wc_Md5Hash(input, length, out_digest) != 0) {
    memset(out_digest, 0, H2_PAL_CRYPTO_MD5_SIZE);
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_hmac_sha1(
    void* user, const uint8_t* key, size_t key_len, const uint8_t* input,
    size_t input_len, uint8_t out_digest[H2_PAL_CRYPTO_SHA1_SIZE]) {
  Hmac hmac;
  word32 key_size;
  word32 input_size;
  int hmac_ready = 0;
  int wolf_rc;
  (void)user;
  if ((key == NULL && key_len != 0u) || (input == NULL && input_len != 0u) ||
      out_digest == NULL ||
      h2_size_to_word32(key_len, &key_size) != H2_PAL_OK ||
      h2_size_to_word32(input_len, &input_size) != H2_PAL_OK) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_digest, 0, H2_PAL_CRYPTO_SHA1_SIZE);
  wolf_rc = wc_HmacInit(&hmac, NULL, INVALID_DEVID);
  if (wolf_rc == 0) {
    hmac_ready = 1;
    wolf_rc = wc_HmacSetKey(&hmac, WC_SHA, key, key_size);
  }
  if (wolf_rc == 0) {
    wolf_rc = wc_HmacUpdate(&hmac, input, input_size);
  }
  if (wolf_rc == 0) {
    wolf_rc = wc_HmacFinal(&hmac, out_digest);
  }
  if (hmac_ready) {
    wc_HmacFree(&hmac);
  }
  if (wolf_rc != 0) {
    memset(out_digest, 0, H2_PAL_CRYPTO_SHA1_SIZE);
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static int h2_wolfcrypt_p256_export(ecc_key* key,
                                    h2_pal_p256_keypair_t* out_keypair) {
  word32 private_len = H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE;
  word32 public_len = H2_PAL_CRYPTO_P256_PUBLIC_KEY_SIZE;
  int wolf_rc = wc_ecc_export_private_only(key, out_keypair->private_key.bytes,
                                           &private_len);
  if (wolf_rc == 0) {
    wolf_rc = wc_ecc_export_x963_ex(key, out_keypair->public_key.bytes,
                                    &public_len, 1);
  }
  if (wolf_rc != 0 || private_len != H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE ||
      public_len != H2_PAL_CRYPTO_P256_PUBLIC_KEY_SIZE) {
    h2_wolfcrypt_secure_zero(out_keypair, sizeof(*out_keypair));
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_p256_keypair_from_private(
    void* user, const h2_pal_p256_private_key_t* private_key,
    h2_pal_p256_keypair_t* out_keypair) {
  ecc_key key;
  WC_RNG rng;
  int key_ready = 0;
  int rng_ready = 0;
  int wolf_rc;
  int rc;
  uint8_t private_copy[H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE];
  (void)user;

  if (out_keypair == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (private_key == NULL) {
    memset(out_keypair, 0, sizeof(*out_keypair));
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!h2_p256_private_key_valid(private_key)) {
    memset(out_keypair, 0, sizeof(*out_keypair));
    return H2_PAL_ERR_FORMAT;
  }
  memcpy(private_copy, private_key->bytes, sizeof(private_copy));
  memset(out_keypair, 0, sizeof(*out_keypair));
  wolf_rc = wc_ecc_init(&key);
  if (wolf_rc != 0) {
    rc = H2_PAL_ERR_IO;
    goto cleanup;
  }
  key_ready = 1;
  wolf_rc = wc_InitRng(&rng);
  if (wolf_rc != 0) {
    rc = H2_PAL_ERR_IO;
    goto cleanup;
  }
  rng_ready = 1;
  wolf_rc = wc_ecc_import_private_key_ex(private_copy,
                                         H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE,
                                         NULL, 0u, &key, ECC_SECP256R1);
  if (wolf_rc != 0) {
    rc = H2_PAL_ERR_FORMAT;
    goto cleanup;
  }
  wolf_rc = h2_wolfcrypt_ecc_make_public(&key, &rng);
  if (wolf_rc != 0) {
    rc = H2_PAL_ERR_IO;
    goto cleanup;
  }
  wolf_rc = wc_ecc_check_key(&key);
  if (wolf_rc != 0) {
    rc = H2_PAL_ERR_FORMAT;
    goto cleanup;
  }
  rc = h2_wolfcrypt_p256_export(&key, out_keypair);

cleanup:
  if (rng_ready) {
    (void)wc_FreeRng(&rng);
  }
  if (key_ready) {
    wc_ecc_free(&key);
  }
  if (rc != H2_PAL_OK) {
    h2_wolfcrypt_secure_zero(out_keypair, sizeof(*out_keypair));
  }
  h2_wolfcrypt_secure_zero(private_copy, sizeof(private_copy));
  return rc;
}

static h2_pal_result_t h2_wolfcrypt_p256_keypair_generate(
    void* user, h2_pal_p256_keypair_t* out_keypair) {
  ecc_key key;
  WC_RNG rng;
  int key_ready = 0;
  int rng_ready = 0;
  int wolf_rc;
  int rc = H2_PAL_ERR_IO;
  (void)user;

  if (out_keypair == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_keypair, 0, sizeof(*out_keypair));
  wolf_rc = wc_InitRng(&rng);
  if (wolf_rc == 0) {
    rng_ready = 1;
    wolf_rc = wc_ecc_init(&key);
  }
  if (wolf_rc == 0) {
    key_ready = 1;
    wolf_rc = wc_ecc_make_key_ex(&rng, H2_PAL_CRYPTO_P256_PRIVATE_KEY_SIZE,
                                 &key, ECC_SECP256R1);
  }
  if (wolf_rc == 0) {
    rc = h2_wolfcrypt_p256_export(&key, out_keypair);
  }
  if (key_ready) {
    wc_ecc_free(&key);
  }
  if (rng_ready) {
    (void)wc_FreeRng(&rng);
  }
  if (rc != H2_PAL_OK) {
    h2_wolfcrypt_secure_zero(out_keypair, sizeof(*out_keypair));
  }
  return rc;
}

static h2_pal_result_t h2_wolfcrypt_p256_public_key_validate(
    void* user, const h2_pal_p256_public_key_t* public_key) {
  ecc_key key;
  int key_ready = 0;
  int wolf_rc;
  (void)user;

  if (public_key == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (public_key->bytes[0] != 0x02u && public_key->bytes[0] != 0x03u) {
    return H2_PAL_ERR_FORMAT;
  }
  wolf_rc = wc_ecc_init(&key);
  if (wolf_rc != 0) {
    return H2_PAL_ERR_IO;
  }
  key_ready = 1;
  wolf_rc = wc_ecc_import_x963_ex(public_key->bytes,
                                  H2_PAL_CRYPTO_P256_PUBLIC_KEY_SIZE, &key,
                                  ECC_SECP256R1);
  if (wolf_rc == 0) {
    wolf_rc = wc_ecc_check_key(&key);
  }
  if (key_ready) {
    wc_ecc_free(&key);
  }
  return wolf_rc == 0 ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t h2_wolfcrypt_p256_public_key_uncompress(
    void* user, const h2_pal_p256_public_key_t* public_key,
    uint8_t out[H2_PAL_CRYPTO_P256_UNCOMPRESSED_PUBLIC_KEY_SIZE]) {
  if (public_key == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, H2_PAL_CRYPTO_P256_UNCOMPRESSED_PUBLIC_KEY_SIZE);
  int rc = h2_wolfcrypt_p256_public_key_validate(user, public_key);
  if (rc != H2_PAL_OK) return rc;
  ecc_key key;
  int wolf_rc = wc_ecc_init(&key);
  if (wolf_rc != 0) return H2_PAL_ERR_IO;
  wolf_rc = wc_ecc_import_x963_ex(public_key->bytes,
                                  H2_PAL_CRYPTO_P256_PUBLIC_KEY_SIZE, &key,
                                  ECC_SECP256R1);
  word32 size = H2_PAL_CRYPTO_P256_UNCOMPRESSED_PUBLIC_KEY_SIZE;
  if (wolf_rc == 0) wolf_rc = wc_ecc_export_x963_ex(&key, out, &size, 0);
  wc_ecc_free(&key);
  if (wolf_rc != 0 || size != H2_PAL_CRYPTO_P256_UNCOMPRESSED_PUBLIC_KEY_SIZE) {
    memset(out, 0, H2_PAL_CRYPTO_P256_UNCOMPRESSED_PUBLIC_KEY_SIZE);
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_p256_shared_secret(
    void* user, const h2_pal_p256_private_key_t* private_key,
    const h2_pal_p256_public_key_t* public_key, uint8_t out[32]) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, 32u);
  if (private_key == NULL || public_key == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!h2_p256_private_key_valid(private_key)) return H2_PAL_ERR_FORMAT;
  int rc = h2_wolfcrypt_p256_public_key_validate(user, public_key);
  if (rc != H2_PAL_OK) return rc;
  ecc_key local, remote;
  if (wc_ecc_init(&local) != 0) return H2_PAL_ERR_IO;
  if (wc_ecc_init(&remote) != 0) {
    wc_ecc_free(&local);
    return H2_PAL_ERR_IO;
  }
  WC_RNG rng;
  int wolf_rc = wc_InitRng(&rng);
  bool rng_ready = wolf_rc == 0;
  if (wolf_rc == 0)
    wolf_rc = wc_ecc_import_private_key_ex(private_key->bytes, 32u, NULL, 0u,
                                           &local, ECC_SECP256R1);
  if (wolf_rc == 0) wolf_rc = wc_ecc_set_rng(&local, &rng);
  if (wolf_rc == 0)
    wolf_rc =
        wc_ecc_import_x963_ex(public_key->bytes, 33u, &remote, ECC_SECP256R1);
  word32 length = 32u;
  if (wolf_rc == 0)
    wolf_rc = wc_ecc_shared_secret(&local, &remote, out, &length);
  wc_ecc_free(&remote);
  wc_ecc_free(&local);
  if (rng_ready) (void)wc_FreeRng(&rng);
  if (wolf_rc != 0 || length != 32u) {
    h2_wolfcrypt_secure_zero(out, 32u);
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static int h2_wolfcrypt_sha256(const uint8_t* message, size_t message_len,
                               uint8_t hash[H2_WOLFCRYPT_SHA256_SIZE]) {
  wc_Sha256 sha;
  word32 message_size;
  int sha_ready = 0;
  int wolf_rc;

  memset(hash, 0, H2_WOLFCRYPT_SHA256_SIZE);
  if (h2_size_to_word32(message_len, &message_size) != H2_PAL_OK) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  wolf_rc = wc_InitSha256(&sha);
  if (wolf_rc == 0) {
    sha_ready = 1;
  }
  if (wolf_rc == 0 && message_len != 0u) {
    wolf_rc = wc_Sha256Update(&sha, message, message_size);
  }
  if (wolf_rc == 0) {
    wolf_rc = wc_Sha256Final(&sha, hash);
  }
  if (sha_ready) {
    wc_Sha256Free(&sha);
  }
  if (wolf_rc != 0) {
    h2_wolfcrypt_secure_zero(hash, H2_WOLFCRYPT_SHA256_SIZE);
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_ecc_sign(
    const uint8_t* private_key, word32 scalar_size, int curve,
    const uint8_t* hash, word32 hash_size, uint8_t* signature,
    int (*signer)(const byte*, word32, WC_RNG*, ecc_key*, mp_int*, mp_int*)) {
  ecc_key key;
  WC_RNG rng;
  mp_int r;
  mp_int s;
  int key_ready = 0;
  int rng_ready = 0;
  int mp_ready = 0;
  int wolf_rc;
  wolf_rc = wc_ecc_init(&key);
  if (wolf_rc == 0) {
    key_ready = 1;
    wolf_rc = wc_InitRng(&rng);
  }
  if (wolf_rc == 0) {
    rng_ready = 1;
    wolf_rc = mp_init_multi(&r, &s, NULL, NULL, NULL, NULL);
  }
  if (wolf_rc == 0) {
    mp_ready = 1;
    wolf_rc = wc_ecc_import_private_key_ex(private_key, scalar_size, NULL, 0u,
                                           &key, curve);
  }
  if (wolf_rc == 0) {
    wolf_rc = signer(hash, hash_size, &rng, &key, &r, &s);
  }
  if (wolf_rc == 0) {
    wolf_rc = mp_to_unsigned_bin_len(&r, signature, scalar_size);
  }
  if (wolf_rc == 0) {
    wolf_rc = mp_to_unsigned_bin_len(&s, signature + scalar_size, scalar_size);
  }
  if (mp_ready) {
    mp_clear(&r);
    mp_clear(&s);
  }
  if (rng_ready) {
    (void)wc_FreeRng(&rng);
  }
  if (key_ready) {
    wc_ecc_free(&key);
  }
  if (wolf_rc != 0) {
    h2_wolfcrypt_secure_zero(signature, 2u * scalar_size);
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_p256_sign(
    void* user, const h2_pal_p256_private_key_t* private_key,
    const uint8_t* message, size_t message_len,
    h2_pal_p256_signature_t* out_signature) {
  (void)user;
  if (out_signature == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_signature, 0, sizeof(*out_signature));
  if (private_key == NULL || (message == NULL && message_len != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_p256_private_key_valid(private_key)) return H2_PAL_ERR_FORMAT;
  uint8_t hash[H2_WOLFCRYPT_SHA256_SIZE];
  int result = h2_wolfcrypt_sha256(message, message_len, hash);
  if (result == H2_PAL_OK)
    result = h2_wolfcrypt_ecc_sign(private_key->bytes, 32u, ECC_SECP256R1, hash,
                                   sizeof(hash), out_signature->bytes,
                                   wc_ecc_sign_hash_ex);
  h2_wolfcrypt_secure_zero(hash, sizeof(hash));
  return result;
}

#ifdef HAVE_ECC_KOBLITZ
static h2_pal_result_t h2_wolfcrypt_secp192k1_sign(
    void* user, const h2_pal_secp192k1_private_key_t* private_key,
    const uint8_t* message, size_t message_len,
    h2_pal_secp192k1_signature_t* out_signature) {
  (void)user;
  if (out_signature == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_signature, 0, sizeof(*out_signature));
  if (private_key == NULL || (message == NULL && message_len != 0u) ||
      message_len > UINT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  static const uint8_t order[24] = {
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
      0x26, 0xf2, 0xfc, 0x17, 0x0f, 0x69, 0x46, 0x6a, 0x74, 0xde, 0xfd, 0x8d};
  if (h2_all_zero(private_key->bytes, sizeof(private_key->bytes)) ||
      memcmp(private_key->bytes, order, sizeof(order)) >= 0)
    return H2_PAL_ERR_FORMAT;
  uint8_t digest[20];
  int result = wc_ShaHash(message, (word32)message_len, digest) == 0
                   ? H2_PAL_OK
                   : H2_PAL_ERR_IO;
  if (result == H2_PAL_OK)
    result = h2_wolfcrypt_ecc_sign(private_key->bytes, 24u, ECC_SECP192K1,
                                   digest, sizeof(digest), out_signature->bytes,
                                   wc_ecc_sign_hash_ex);
  h2_wolfcrypt_secure_zero(digest, sizeof(digest));
  return result;
}
#endif

#ifdef WOLFSSL_SM2
static bool h2_wolfcrypt_sm2_scalar_valid(const uint8_t bytes[32],
                                          bool private_key) {
  static const uint8_t order[32] = {
      0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0x72, 0x03, 0xdf, 0x6b, 0x21, 0xc6,
      0x05, 0x2b, 0x53, 0xbb, 0xf4, 0x09, 0x39, 0xd5, 0x41, 0x23};
  return !h2_all_zero(bytes, 32u) && memcmp(bytes, order, sizeof(order)) < 0 &&
         !(private_key && memcmp(bytes, order, 31u) == 0 && bytes[31] == 0x22u);
}

static h2_pal_result_t h2_wolfcrypt_sm2_public(
    void* user, const h2_pal_sm2_private_key_t* private_key,
    h2_pal_sm2_public_key_t* public_key) {
  (void)user;
  if (public_key == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(public_key, 0, sizeof(*public_key));
  if (private_key == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!h2_wolfcrypt_sm2_scalar_valid(private_key->bytes, true))
    return H2_PAL_ERR_FORMAT;
  ecc_key key;
  WC_RNG rng;
  bool key_ready = false, rng_ready = false;
  int rc = wc_ecc_init(&key);
  key_ready = rc == 0;
  if (rc == 0) {
    rc = wc_InitRng(&rng);
    rng_ready = rc == 0;
  }
  if (rc == 0)
    rc = wc_ecc_import_private_key_ex(private_key->bytes, 32u, NULL, 0u, &key,
                                      ECC_SM2P256V1);
  if (rc == 0) rc = h2_wolfcrypt_ecc_make_public(&key, &rng);
  word32 length = sizeof(public_key->bytes);
  if (rc == 0) rc = wc_ecc_export_x963(&key, public_key->bytes, &length);
  if (rng_ready) (void)wc_FreeRng(&rng);
  if (key_ready) wc_ecc_free(&key);
  if (rc != 0 || length != sizeof(public_key->bytes)) {
    memset(public_key, 0, sizeof(*public_key));
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_wolfcrypt_sm2_sign(
    void* user, const h2_pal_sm2_private_key_t* private_key,
    const uint8_t digest[32], h2_pal_sm2_signature_t* signature) {
  (void)user;
  if (signature == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(signature, 0, sizeof(*signature));
  if (private_key == NULL || digest == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!h2_wolfcrypt_sm2_scalar_valid(private_key->bytes, true))
    return H2_PAL_ERR_FORMAT;
  return h2_wolfcrypt_ecc_sign(private_key->bytes, 32u, ECC_SM2P256V1, digest,
                               32u, signature->bytes, wc_ecc_sm2_sign_hash_ex);
}

#endif

static h2_pal_result_t h2_wolfcrypt_p256_verify(
    void* user, const h2_pal_p256_public_key_t* public_key,
    const uint8_t* message, size_t message_len,
    const h2_pal_p256_signature_t* signature) {
  uint8_t hash[H2_WOLFCRYPT_SHA256_SIZE];
  ecc_key key;
  mp_int r;
  mp_int s;
  int key_ready = 0;
  int mp_ready = 0;
  int verified = 0;
  int wolf_rc;
  int rc;
  (void)user;

  if (public_key == NULL || (message == NULL && message_len != 0u) ||
      signature == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = h2_wolfcrypt_p256_public_key_validate(NULL, public_key);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (!h2_p256_scalar_valid(signature->bytes) ||
      !h2_p256_scalar_valid(signature->bytes +
                            H2_PAL_CRYPTO_P256_SIGNATURE_SIZE / 2u)) {
    return H2_PAL_ERR_FORMAT;
  }
  rc = h2_wolfcrypt_sha256(message, message_len, hash);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  wolf_rc = wc_ecc_init(&key);
  if (wolf_rc == 0) {
    key_ready = 1;
    wolf_rc = mp_init_multi(&r, &s, NULL, NULL, NULL, NULL);
  }
  if (wolf_rc == 0) {
    mp_ready = 1;
    wolf_rc = wc_ecc_import_x963_ex(public_key->bytes,
                                    H2_PAL_CRYPTO_P256_PUBLIC_KEY_SIZE, &key,
                                    ECC_SECP256R1);
  }
  if (wolf_rc == 0) {
    wolf_rc = mp_read_unsigned_bin(&r, signature->bytes,
                                   H2_PAL_CRYPTO_P256_SIGNATURE_SIZE / 2u);
  }
  if (wolf_rc == 0) {
    wolf_rc = mp_read_unsigned_bin(
        &s, signature->bytes + H2_PAL_CRYPTO_P256_SIGNATURE_SIZE / 2u,
        H2_PAL_CRYPTO_P256_SIGNATURE_SIZE / 2u);
  }
  if (wolf_rc == 0) {
    wolf_rc =
        wc_ecc_verify_hash_ex(&r, &s, hash, sizeof(hash), &verified, &key);
  }
  if (mp_ready) {
    mp_clear(&r);
    mp_clear(&s);
  }
  if (key_ready) {
    wc_ecc_free(&key);
  }
  h2_wolfcrypt_secure_zero(hash, sizeof(hash));
  if (wolf_rc != 0) {
    return H2_PAL_ERR_IO;
  }
  return verified == 1 ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

static const h2_pal_crypto_vtable_t h2_wolfcrypt_vtable = {
    .random = h2_wolfcrypt_random,
    .x25519_keypair_generate = h2_wolfcrypt_x25519_keypair_generate,
    .x25519_public_key_from_private =
        h2_wolfcrypt_x25519_public_key_from_private,
    .x25519_shared_secret = h2_wolfcrypt_x25519_shared_secret,
    .hkdf_sha256 = h2_wolfcrypt_hkdf_sha256,
    .aead_seal = h2_wolfcrypt_aead_seal,
    .aead_open = h2_wolfcrypt_aead_open,
    .aes_ctr_xor = h2_wolfcrypt_aes_ctr_xor,
    .md5 = h2_wolfcrypt_md5,
    .hmac_sha1 = h2_wolfcrypt_hmac_sha1,
    .p256_keypair_from_private = h2_wolfcrypt_p256_keypair_from_private,
    .p256_keypair_generate = h2_wolfcrypt_p256_keypair_generate,
    .p256_public_key_validate = h2_wolfcrypt_p256_public_key_validate,
    .ecdsa_p256_sha256_sign = h2_wolfcrypt_p256_sign,
    .ecdsa_p256_sha256_verify = h2_wolfcrypt_p256_verify,
    .p256_public_key_uncompress = h2_wolfcrypt_p256_public_key_uncompress,
    .p256_shared_secret = h2_wolfcrypt_p256_shared_secret,
    .aes_cbc = h2_wolfcrypt_aes_cbc,
#ifndef NO_DES3
    .des_cbc = h2_wolfcrypt_des_cbc,
#endif
#ifdef HAVE_ECC_KOBLITZ
    .ecdsa_secp192k1_sha1_sign = h2_wolfcrypt_secp192k1_sign,
#endif
#ifdef WOLFSSL_SM2
    .sm2_public_key_from_private = h2_wolfcrypt_sm2_public,
    .sm2_sign_digest = h2_wolfcrypt_sm2_sign,
#endif
};

static const h2_pal_crypto_api_t h2_wolfcrypt_api = {
    .user = NULL,
    .vtable = &h2_wolfcrypt_vtable,
};

int h2_wolfcrypt_crypto_init(const h2_wolfcrypt_crypto_config_t* config) {
  if (config == NULL || config->entropy == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (h2_wolfcrypt_state.ready) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_wolfcrypt_state.entropy_user = config->entropy_user;
  h2_wolfcrypt_state.entropy = config->entropy;
  h2_wolfcrypt_state.ready = 1;
  return H2_PAL_OK;
}

void h2_wolfcrypt_crypto_deinit(void) {
  h2_wolfcrypt_secure_zero(&h2_wolfcrypt_state, sizeof(h2_wolfcrypt_state));
}

const h2_pal_crypto_api_t* h2_wolfcrypt_crypto_api(void) {
  return h2_wolfcrypt_state.ready ? &h2_wolfcrypt_api
                                  : h2_pal_unsupported_crypto_api();
}
