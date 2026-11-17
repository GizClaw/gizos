#include "h2_crypto.h"

#include <string.h>

static void des_wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  while (length-- != 0u) *bytes++ = 0u;
}

h2_pal_result_t h2_crypto_des_cbc(const h2_pal_crypto_api_t* crypto,
                                  h2_pal_crypto_cipher_direction_t direction,
                                  const uint8_t* key, size_t key_length,
                                  const uint8_t iv[8], const uint8_t* input,
                                  size_t length, h2_pal_crypto_buf_t* output) {
  return h2_pal_crypto_des_cbc(crypto, direction, key, key_length, iv, input,
                               length, output);
}

h2_pal_result_t h2_crypto_iso9797_alg3_mac(const h2_pal_crypto_api_t* crypto,
                                           const uint8_t key[16],
                                           const uint8_t* message,
                                           size_t length, uint8_t output[8]) {
  if (output == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(output, 0, 8u);
  if (key == NULL || (message == NULL && length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t chain[8] = {0}, block[8] = {0}, next[8] = {0};
  size_t offset = 0u;
  int rc = H2_PAL_OK;
  while (length - offset >= 8u) {
    h2_pal_crypto_buf_t encrypted = {next, 0u, sizeof(next)};
    rc = h2_crypto_des_cbc(crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT, key, 8u, chain,
                           message + offset, 8u, &encrypted);
    if (rc != H2_PAL_OK) break;
    if (encrypted.len != 8u) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    memcpy(chain, next, sizeof(chain));
    offset += 8u;
  }
  if (rc == H2_PAL_OK) {
    size_t remaining = length - offset;
    if (remaining != 0u) memcpy(block, message + offset, remaining);
    block[remaining] = 0x80u;
    h2_pal_crypto_buf_t encrypted = {next, 0u, sizeof(next)};
    rc = h2_crypto_des_cbc(crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT, key, 16u,
                           chain, block, sizeof(block), &encrypted);
    if (rc == H2_PAL_OK && encrypted.len != 8u) rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK) memcpy(output, next, 8u);
  }
  des_wipe(chain, sizeof(chain));
  des_wipe(block, sizeof(block));
  des_wipe(next, sizeof(next));
  return rc;
}
