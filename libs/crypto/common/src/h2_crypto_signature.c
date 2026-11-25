#include "h2_crypto.h"

#include <string.h>

h2_pal_result_t h2_crypto_ecdsa_der_encode(const uint8_t* signature,
                                           size_t scalar_length,
                                           uint8_t* output, size_t capacity,
                                           size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (signature == NULL || output == NULL || scalar_length == 0u ||
      scalar_length > 32u)
    return H2_PAL_ERR_INVALID_ARG;
  size_t offsets[2] = {0u, scalar_length};
  size_t sizes[2] = {scalar_length, scalar_length};
  bool pads[2];
  size_t required = 6u;
  for (size_t part = 0u; part < 2u; ++part) {
    while (sizes[part] > 1u && signature[offsets[part]] == 0u) {
      ++offsets[part];
      --sizes[part];
    }
    pads[part] = (signature[offsets[part]] & 0x80u) != 0u;
    required += sizes[part] + (pads[part] ? 1u : 0u);
  }
  if (capacity < required) return H2_PAL_ERR_NO_SPACE;
  uint8_t encoded[72];
  encoded[0] = 0x30u;
  encoded[1] = (uint8_t)(required - 2u);
  size_t written = 2u;
  for (size_t part = 0u; part < 2u; ++part) {
    encoded[written++] = 2u;
    encoded[written++] = (uint8_t)(sizes[part] + (pads[part] ? 1u : 0u));
    if (pads[part]) encoded[written++] = 0u;
    memcpy(encoded + written, signature + offsets[part], sizes[part]);
    written += sizes[part];
  }
  memcpy(output, encoded, written);
  *length = written;
  return H2_PAL_OK;
}

h2_pal_result_t h2_crypto_ecdsa_p256_der_encode(
    const h2_pal_p256_signature_t* signature, uint8_t* output, size_t capacity,
    size_t* length) {
  return h2_crypto_ecdsa_der_encode(signature != NULL ? signature->bytes : NULL,
                                    32u, output, capacity, length);
}
