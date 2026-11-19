#include "h2_crypto.h"

#include "h2_sm3.h"
#include <string.h>

static void sm2_wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  while (length-- != 0u) *bytes++ = 0u;
}

static int sm2_digest(const h2_pal_sm2_public_key_t* public_key,
                      const uint8_t* identity, size_t identity_length,
                      const uint8_t* message, size_t message_length,
                      uint8_t output[32]) {
  if (public_key == NULL || public_key->bytes[0] != 4u ||
      identity_length > 8191u || (identity == NULL && identity_length != 0u) ||
      (message == NULL && message_length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  static const uint8_t curve[128] = {
      0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfc, 0x28, 0xe9, 0xfa, 0x9e,
      0x9d, 0x9f, 0x5e, 0x34, 0x4d, 0x5a, 0x9e, 0x4b, 0xcf, 0x65, 0x09, 0xa7,
      0xf3, 0x97, 0x89, 0xf5, 0x15, 0xab, 0x8f, 0x92, 0xdd, 0xbc, 0xbd, 0x41,
      0x4d, 0x94, 0x0e, 0x93, 0x32, 0xc4, 0xae, 0x2c, 0x1f, 0x19, 0x81, 0x19,
      0x5f, 0x99, 0x04, 0x46, 0x6a, 0x39, 0xc9, 0x94, 0x8f, 0xe3, 0x0b, 0xbf,
      0xf2, 0x66, 0x0b, 0xe1, 0x71, 0x5a, 0x45, 0x89, 0x33, 0x4c, 0x74, 0xc7,
      0xbc, 0x37, 0x36, 0xa2, 0xf4, 0xf6, 0x77, 0x9c, 0x59, 0xbd, 0xce, 0xe3,
      0x6b, 0x69, 0x21, 0x53, 0xd0, 0xa9, 0x87, 0x7c, 0xc6, 0x2a, 0x47, 0x40,
      0x02, 0xdf, 0x32, 0xe5, 0x21, 0x39, 0xf0, 0xa0};
  uint8_t identity_bits[2] = {(uint8_t)(identity_length >> 5u),
                              (uint8_t)(identity_length << 3u)};
  uint8_t binding[32];
  h2_sm3_t digest;
  h2_sm3_init(&digest);
  h2_sm3_update(&digest, identity_bits, sizeof(identity_bits));
  h2_sm3_update(&digest, identity, identity_length);
  h2_sm3_update(&digest, curve, sizeof(curve));
  h2_sm3_update(&digest, public_key->bytes + 1u, 64u);
  h2_sm3_finish(&digest, binding);
  h2_sm3_init(&digest);
  h2_sm3_update(&digest, binding, sizeof(binding));
  h2_sm3_update(&digest, message, message_length);
  h2_sm3_finish(&digest, output);
  sm2_wipe(binding, sizeof(binding));
  return H2_PAL_OK;
}

h2_pal_result_t h2_crypto_sm2_sign(const h2_pal_crypto_api_t* crypto,
                                   const h2_pal_sm2_private_key_t* private_key,
                                   const uint8_t* identity,
                                   size_t identity_length,
                                   const uint8_t* message,
                                   size_t message_length,
                                   h2_pal_sm2_signature_t* signature) {
  if (signature == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(signature, 0, sizeof(*signature));
  if (private_key == NULL || identity_length > 8191u ||
      (identity == NULL && identity_length != 0u) ||
      (message == NULL && message_length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_sm2_public_key_t public_key;
  uint8_t digest[32] = {0};
  int rc = h2_pal_crypto_sm2_public_key_from_private(crypto, private_key,
                                                     &public_key);
  if (rc == H2_PAL_OK)
    rc = sm2_digest(&public_key, identity, identity_length, message,
                    message_length, digest);
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_sm2_sign_digest(crypto, private_key, digest, signature);
  sm2_wipe(digest, sizeof(digest));
  sm2_wipe(&public_key, sizeof(public_key));
  return rc;
}
