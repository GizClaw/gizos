#include "h2_crypto.h"

#include <stdbool.h>
#include <string.h>

static void wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  for (size_t index = 0u; index < length; ++index) bytes[index] = 0u;
}

bool h2_crypto_bytes_equal(const uint8_t* first, const uint8_t* second,
                           size_t length) {
  if (length != 0u && (first == NULL || second == NULL)) return false;
  volatile unsigned difference = 0u;
  for (size_t index = 0u; index < length; ++index)
    difference |= first[index] ^ second[index];
  return difference == 0u;
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
  int rc =
      h2_pal_crypto_p256_public_key_uncompress(crypto, &compressed, expanded);
  if (rc == H2_PAL_OK) {
    if (!h2_crypto_bytes_equal(input, expanded, sizeof(expanded)))
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
  int rc = h2_pal_crypto_aes_cbc(crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT, key,
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
  int rc = h2_pal_crypto_aes_cbc(crypto, H2_PAL_CRYPTO_CIPHER_DECRYPT, key,
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
  int rc = h2_pal_crypto_hmac_sha1(crypto, key, key_length, encoded,
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
