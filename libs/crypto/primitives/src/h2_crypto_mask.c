#include "h2_crypto.h"

h2_pal_result_t h2_crypto_xor_mask(const uint8_t* input, size_t length,
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

bool h2_crypto_bytes_equal(const uint8_t* first, const uint8_t* second,
                           size_t length) {
  if (length != 0u && (first == NULL || second == NULL)) return false;
  volatile unsigned difference = 0u;
  for (size_t index = 0u; index < length; ++index)
    difference |= first[index] ^ second[index];
  return difference == 0u;
}

h2_pal_result_t h2_crypto_hash31(const uint8_t* input, size_t length,
                                 uint64_t* output) {
  if (output == NULL || (input == NULL && length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t hash = 0u;
  for (size_t index = 0u; index < length; ++index)
    hash = hash * 31u + input[index];
  *output = hash;
  return H2_PAL_OK;
}
