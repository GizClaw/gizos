#include "h2_crypto.h"

#include "h2_sha256.h"
#include "h2_sm3.h"
#include <string.h>

h2_pal_result_t h2_crypto_sha256(const uint8_t* input, size_t length,
                                 uint8_t output[32]) {
  if (output == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(output, 0, 32u);
  if (input == NULL && length != 0u) return H2_PAL_ERR_INVALID_ARG;
  h2_sha256_t digest;
  h2_sha256_init(&digest);
  h2_sha256_update(&digest, input, length);
  h2_sha256_finish(&digest, output);
  return H2_PAL_OK;
}

h2_pal_result_t h2_crypto_sm3(const uint8_t* input, size_t length,
                              uint8_t output[32]) {
  if (output == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(output, 0, 32u);
  if (input == NULL && length != 0u) return H2_PAL_ERR_INVALID_ARG;
  h2_sm3_t digest;
  h2_sm3_init(&digest);
  h2_sm3_update(&digest, input, length);
  h2_sm3_finish(&digest, output);
  return H2_PAL_OK;
}
