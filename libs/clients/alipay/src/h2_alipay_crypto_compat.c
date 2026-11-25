#include "h2_alipay_crypto_compat.h"

#include "h2_crypto.h"
#include <string.h>

static void wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  for (size_t index = 0u; index < length; ++index) bytes[index] = 0u;
}

h2_pal_result_t h2_alipay_xor_mask(const uint8_t* input, size_t length,
                                   const uint8_t* mask, size_t mask_length,
                                   bool reverse, uint8_t* output) {
  if (mask == NULL || mask_length == 0u ||
      (length != 0u && (input == NULL || output == NULL)))
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t index = 0u; index < length; ++index) {
    size_t position = index % mask_length;
    if (reverse) position = mask_length - 1u - position;
    output[index] = input[index] ^ mask[position];
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_hash31(const uint8_t* input, size_t length,
                                 uint64_t* output) {
  if (output == NULL || (input == NULL && length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t hash = 0u;
  for (size_t index = 0u; index < length; ++index)
    hash = hash * 31u + input[index];
  *output = hash;
  return H2_PAL_OK;
}

struct h2_alipay_md5_aes_keyset {
  const h2_pal_mem_api_t* memory;
  const h2_pal_crypto_api_t* crypto;
  size_t count;
  uint8_t keys[][16];
};

h2_pal_result_t h2_alipay_md5_aes_keyset_open(
    const h2_pal_mem_api_t* memory, const h2_pal_crypto_api_t* crypto,
    const h2_alipay_key_material_t* materials, size_t count,
    h2_alipay_md5_aes_keyset_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (memory == NULL || crypto == NULL || materials == NULL || count == 0u ||
      count > (SIZE_MAX - sizeof(h2_alipay_md5_aes_keyset_t)) / 16u)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t index = 0u; index < count; ++index)
    if (materials[index].data == NULL && materials[index].length != 0u)
      return H2_PAL_ERR_INVALID_ARG;
  size_t size = sizeof(h2_alipay_md5_aes_keyset_t) + count * 16u;
  h2_alipay_md5_aes_keyset_t* keys = h2_pal_mem_alloc(memory, size);
  if (keys == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(keys, 0, size);
  keys->memory = memory;
  keys->crypto = crypto;
  keys->count = count;
  static const char alphabet[] = "0123456789abcdef";
  uint8_t digest[16] = {0};
  int rc = H2_PAL_OK;
  for (size_t index = 0u; index < count; ++index) {
    rc = h2_pal_crypto_md5(crypto, materials[index].data,
                           materials[index].length, digest);
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
    h2_alipay_md5_aes_keyset_close(keys);
  return rc;
}

h2_pal_result_t h2_alipay_md5_aes_keyset_encrypt(
    h2_alipay_md5_aes_keyset_t* keys, size_t index, const uint8_t* input,
    size_t input_length, uint8_t* output, size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (keys == NULL || index >= keys->count) return H2_PAL_ERR_INVALID_ARG;
  return h2_crypto_aes_cbc_pkcs7_encrypt(
      keys->crypto, keys->memory, keys->keys[index], 16u, keys->keys[index],
      input, input_length, output, capacity, length);
}

h2_pal_result_t h2_alipay_md5_aes_keyset_decrypt(
    h2_alipay_md5_aes_keyset_t* keys, size_t index, const uint8_t* input,
    size_t input_length, uint8_t* output, size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (keys == NULL || index >= keys->count) return H2_PAL_ERR_INVALID_ARG;
  return h2_crypto_aes_cbc_pkcs7_decrypt(keys->crypto, keys->keys[index], 16u,
                                         keys->keys[index], input, input_length,
                                         output, capacity, length);
}

void h2_alipay_md5_aes_keyset_close(h2_alipay_md5_aes_keyset_t* keys) {
  if (keys == NULL) return;
  const h2_pal_mem_api_t* memory = keys->memory;
  size_t size = sizeof(*keys) + keys->count * 16u;
  wipe(keys, size);
  h2_pal_mem_free(memory, keys);
}
