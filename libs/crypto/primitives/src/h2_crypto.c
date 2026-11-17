#include "h2_crypto.h"

#include <stdbool.h>
#include <string.h>

static void wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  for (size_t index = 0u; index < length; ++index) bytes[index] = 0u;
}

h2_pal_result_t h2_crypto_random(const h2_pal_crypto_api_t* crypto,
                                 uint8_t* output, size_t length) {
  return h2_pal_crypto_random(crypto, output, length);
}

h2_pal_result_t h2_crypto_md5(const h2_pal_crypto_api_t* crypto,
                              const uint8_t* data, size_t length,
                              uint8_t output[16]) {
  return h2_pal_crypto_md5(crypto, data, length, output);
}

h2_pal_result_t h2_crypto_hmac_sha1(const h2_pal_crypto_api_t* crypto,
                                    const uint8_t* key, size_t key_length,
                                    const uint8_t* data, size_t length,
                                    uint8_t output[20]) {
  return h2_pal_crypto_hmac_sha1(crypto, key, key_length, data, length, output);
}

h2_pal_result_t h2_crypto_sha1(const h2_pal_crypto_api_t* crypto,
                               const uint8_t* input, size_t length,
                               uint8_t output[20]) {
  return h2_pal_crypto_sha1(crypto, input, length, output);
}

h2_pal_result_t h2_crypto_hmac_md5(const h2_pal_crypto_api_t* crypto,
                                   const uint8_t* key, size_t key_length,
                                   const uint8_t* input, size_t length,
                                   uint8_t output[16]) {
  return h2_pal_crypto_hmac_md5(crypto, key, key_length, input, length, output);
}

h2_pal_result_t h2_crypto_aes_cbc(const h2_pal_crypto_api_t* crypto,
                                  h2_pal_crypto_cipher_direction_t direction,
                                  const uint8_t* key, size_t key_length,
                                  const uint8_t iv[16], const uint8_t* input,
                                  size_t length, h2_pal_crypto_buf_t* output) {
  return h2_pal_crypto_aes_cbc(crypto, direction, key, key_length, iv, input,
                               length, output);
}

h2_pal_result_t h2_crypto_p256_keypair_generate(
    const h2_pal_crypto_api_t* crypto, h2_pal_p256_keypair_t* output) {
  return h2_pal_crypto_p256_keypair_generate(crypto, output);
}

h2_pal_result_t h2_crypto_p256_public_key_export(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_p256_public_key_t* public_key, uint8_t output[65]) {
  return h2_pal_crypto_p256_public_key_uncompress(crypto, public_key, output);
}

h2_pal_result_t h2_crypto_p256_shared_secret(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_p256_private_key_t* private_key,
    const h2_pal_p256_public_key_t* public_key, uint8_t output[32]) {
  return h2_pal_crypto_p256_shared_secret(crypto, private_key, public_key,
                                          output);
}

h2_pal_result_t h2_crypto_ecdsa_p256_sha256_verify(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_p256_public_key_t* public_key, const uint8_t* message,
    size_t message_length, const h2_pal_p256_signature_t* signature) {
  return h2_pal_crypto_ecdsa_p256_sha256_verify(crypto, public_key, message,
                                                message_length, signature);
}

h2_pal_result_t h2_crypto_ecdsa_p256_sha256_sign(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_p256_private_key_t* private_key, const uint8_t* message,
    size_t message_length, h2_pal_p256_signature_t* signature) {
  return h2_pal_crypto_ecdsa_p256_sha256_sign(crypto, private_key, message,
                                              message_length, signature);
}

h2_pal_result_t h2_crypto_p256_public_key_import(
    const h2_pal_crypto_api_t* crypto, const uint8_t input[65],
    h2_pal_p256_public_key_t* output) {
  if (input == NULL || output == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (input[0] != 4u) return H2_PAL_ERR_FORMAT;
  h2_pal_p256_public_key_t compressed;
  compressed.bytes[0] = (uint8_t)(2u | (input[64] & 1u));
  memcpy(compressed.bytes + 1u, input + 1u, 32u);
  uint8_t expanded[65];
  int rc = h2_crypto_p256_public_key_export(crypto, &compressed, expanded);
  if (rc == H2_PAL_OK) {
    unsigned difference = 0u;
    for (size_t index = 0u; index < sizeof(expanded); ++index)
      difference |= input[index] ^ expanded[index];
    if (difference != 0u)
      rc = H2_PAL_ERR_FORMAT;
    else
      *output = compressed;
  }
  return rc;
}

static bool valid_key(const uint8_t* key, size_t length) {
  return key != NULL && (length == 16u || length == 24u || length == 32u);
}

h2_pal_result_t h2_crypto_aes_cbc_pkcs7_encrypt(
    const h2_pal_crypto_api_t* crypto, const h2_pal_mem_api_t* memory,
    const uint8_t* key, size_t key_length, const uint8_t iv[16],
    const uint8_t* input, size_t input_length, uint8_t* output, size_t capacity,
    size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (!valid_key(key, key_length) || iv == NULL || output == NULL ||
      memory == NULL || (input == NULL && input_length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  size_t padding = 16u - input_length % 16u;
  if (input_length > SIZE_MAX - padding) return H2_PAL_ERR_INVALID_ARG;
  size_t size = input_length + padding;
  if (capacity < size) return H2_PAL_ERR_NO_SPACE;
  uint8_t* padded = h2_pal_mem_alloc(memory, size);
  if (padded == NULL) return H2_PAL_ERR_NO_MEMORY;
  if (input_length != 0u) memcpy(padded, input, input_length);
  memset(padded + input_length, (int)padding, padding);
  h2_pal_crypto_buf_t buffer = {.data = output, .cap = capacity};
  int rc = h2_crypto_aes_cbc(crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT, key,
                             key_length, iv, padded, size, &buffer);
  if (rc == H2_PAL_OK && buffer.len != size) rc = H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK)
    *length = size;
  else
    wipe(output, size);
  wipe(padded, size);
  h2_pal_mem_free(memory, padded);
  return rc;
}

h2_pal_result_t h2_crypto_aes_cbc_pkcs7_decrypt(
    const h2_pal_crypto_api_t* crypto, const uint8_t* key, size_t key_length,
    const uint8_t iv[16], const uint8_t* input, size_t input_length,
    uint8_t* output, size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (!valid_key(key, key_length) || iv == NULL || input == NULL ||
      output == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (input_length == 0u || input_length % 16u != 0u) return H2_PAL_ERR_FORMAT;
  if (capacity < input_length) return H2_PAL_ERR_NO_SPACE;
  h2_pal_crypto_buf_t buffer = {.data = output, .cap = capacity};
  int rc = h2_crypto_aes_cbc(crypto, H2_PAL_CRYPTO_CIPHER_DECRYPT, key,
                             key_length, iv, input, input_length, &buffer);
  if (rc == H2_PAL_OK && buffer.len != input_length) rc = H2_PAL_ERR_IO;
  size_t padding = 0u;
  if (rc == H2_PAL_OK) {
    padding = output[input_length - 1u];
    unsigned difference = padding == 0u || padding > 16u;
    for (size_t index = 0u; index < 16u; ++index) {
      unsigned mask = 0u - (unsigned)(index < padding);
      difference |=
          mask & (output[input_length - index - 1u] ^ (unsigned)padding);
    }
    if (difference != 0u) rc = H2_PAL_ERR_FORMAT;
  }
  if (rc == H2_PAL_OK)
    *length = input_length - padding;
  else
    wipe(output, input_length);
  return rc;
}

h2_pal_result_t h2_crypto_hotp_sha1(const h2_pal_crypto_api_t* crypto,
                                    const uint8_t* key, size_t key_length,
                                    uint64_t counter, unsigned digits,
                                    uint32_t* output) {
  if (key == NULL || key_length == 0u || output == NULL || digits < 6u ||
      digits > 9u)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t encoded[8], digest[20] = {0};
  for (size_t index = 0u; index < sizeof(encoded); ++index) {
    encoded[sizeof(encoded) - index - 1u] = (uint8_t)counter;
    counter >>= 8u;
  }
  int rc = h2_crypto_hmac_sha1(crypto, key, key_length, encoded,
                               sizeof(encoded), digest);
  if (rc == H2_PAL_OK) {
    size_t offset = digest[19] & 15u;
    uint32_t value = ((uint32_t)(digest[offset] & 0x7fu) << 24u) |
                     ((uint32_t)digest[offset + 1u] << 16u) |
                     ((uint32_t)digest[offset + 2u] << 8u) |
                     digest[offset + 3u];
    uint32_t modulus = 1u;
    for (unsigned index = 0u; index < digits; ++index) modulus *= 10u;
    *output = value % modulus;
  }
  wipe(encoded, sizeof(encoded));
  wipe(digest, sizeof(digest));
  return rc;
}

struct h2_crypto_md5_aes_keyset {
  const h2_pal_mem_api_t* memory;
  const h2_pal_crypto_api_t* crypto;
  size_t count;
  uint8_t keys[][16];
};

h2_pal_result_t h2_crypto_md5_aes_keyset_open(
    const h2_pal_mem_api_t* memory, const h2_pal_crypto_api_t* crypto,
    const h2_crypto_key_material_t* materials, size_t count,
    h2_crypto_md5_aes_keyset_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (memory == NULL || crypto == NULL || materials == NULL || count == 0u ||
      count > (SIZE_MAX - sizeof(h2_crypto_md5_aes_keyset_t)) / 16u)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t index = 0u; index < count; ++index)
    if (materials[index].data == NULL && materials[index].length != 0u)
      return H2_PAL_ERR_INVALID_ARG;
  size_t size = sizeof(h2_crypto_md5_aes_keyset_t) + count * 16u;
  h2_crypto_md5_aes_keyset_t* keys = h2_pal_mem_alloc(memory, size);
  if (keys == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(keys, 0, size);
  keys->memory = memory;
  keys->crypto = crypto;
  keys->count = count;
  static const char alphabet[] = "0123456789abcdef";
  uint8_t digest[16] = {0};
  int rc = H2_PAL_OK;
  for (size_t index = 0u; index < count; ++index) {
    rc = h2_crypto_md5(crypto, materials[index].data, materials[index].length,
                       digest);
    if (rc != H2_PAL_OK) break;
    for (size_t byte = 0u; byte < 8u; ++byte) {
      keys->keys[index][byte * 2u] = (uint8_t)alphabet[digest[byte] >> 4u];
      keys->keys[index][byte * 2u + 1u] = (uint8_t)alphabet[digest[byte] & 15u];
    }
  }
  wipe(digest, sizeof(digest));
  if (rc == H2_PAL_OK)
    *out = keys;
  else
    h2_crypto_md5_aes_keyset_close(keys);
  return rc;
}

h2_pal_result_t h2_crypto_md5_aes_keyset_encrypt(
    h2_crypto_md5_aes_keyset_t* keys, size_t index, const uint8_t* input,
    size_t input_length, uint8_t* output, size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (keys == NULL || index >= keys->count) return H2_PAL_ERR_INVALID_ARG;
  return h2_crypto_aes_cbc_pkcs7_encrypt(
      keys->crypto, keys->memory, keys->keys[index], 16u, keys->keys[index],
      input, input_length, output, capacity, length);
}

h2_pal_result_t h2_crypto_md5_aes_keyset_decrypt(
    h2_crypto_md5_aes_keyset_t* keys, size_t index, const uint8_t* input,
    size_t input_length, uint8_t* output, size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (keys == NULL || index >= keys->count) return H2_PAL_ERR_INVALID_ARG;
  return h2_crypto_aes_cbc_pkcs7_decrypt(keys->crypto, keys->keys[index], 16u,
                                         keys->keys[index], input, input_length,
                                         output, capacity, length);
}

void h2_crypto_md5_aes_keyset_close(h2_crypto_md5_aes_keyset_t* keys) {
  if (keys == NULL) return;
  const h2_pal_mem_api_t* memory = keys->memory;
  size_t size = sizeof(*keys) + keys->count * 16u;
  wipe(keys, size);
  h2_pal_mem_free(memory, keys);
}
