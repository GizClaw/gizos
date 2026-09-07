#ifndef H2_APP_TEST_CRYPTO_H
#define H2_APP_TEST_CRYPTO_H
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2_app_test_fault.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Scripted crypto boundary for identity/preferences tests, not cryptography.
 * Supply known keypair/byte fixtures explicitly. No fabricated key derivation,
 * network security or entropy claim is made. The random byte span is borrowed;
 * exhaustion returns NO_SPACE without consuming partial data. */
typedef struct h2_app_test_crypto {
  h2_pal_crypto_api_t api;
  const uint8_t *random_bytes;
  size_t random_size, random_offset;
  bool keypair_ready;
  h2_pal_x25519_keypair_t keypair;
  h2_app_test_fault_t random, generate, derive;
} h2_app_test_crypto_t;
/** Initialize empty caller storage; key operations remain UNSUPPORTED until
 * keypair_ready is set. No allocation or deinit needed. */
void h2_app_test_crypto_init(h2_app_test_crypto_t *crypto);

#ifdef __cplusplus
}
#endif
#endif
