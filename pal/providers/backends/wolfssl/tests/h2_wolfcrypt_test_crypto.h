#ifndef H2_WOLFCRYPT_TEST_CRYPTO_H
#define H2_WOLFCRYPT_TEST_CRYPTO_H

#include "os/h2_pal_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Test-only nonce injection for cross-implementation known-answer fixtures.
 * Uses the real wolfCrypt signer and the initialized provider's blinding RNG.
 * Inputs remain borrowed through this synchronous call. */
h2_pal_result_t h2_wolfcrypt_test_p256_sign(
    const uint8_t nonce[32], const h2_pal_p256_private_key_t* private_key,
    const uint8_t* message, size_t length, h2_pal_p256_signature_t* signature);

/** Test-only legacy secp192k1 nonce control, using the upstream signer.
 * The nonce must be a valid 24-byte scalar. Successful signatures are also
 * verified by the real upstream verifier with the derived public key. */
h2_pal_result_t h2_wolfcrypt_test_secp192k1_sign(
    const uint8_t nonce[24], const h2_pal_secp192k1_private_key_t* private_key,
    const uint8_t* message, size_t length,
    h2_pal_secp192k1_signature_t* signature);

/** Verify a legacy signature using the public key derived from a synthetic
 * fixture's private scalar. This helper is testonly and retains no key. */
h2_pal_result_t h2_wolfcrypt_test_secp192k1_verify(
    const h2_pal_secp192k1_private_key_t* private_key, const uint8_t* message,
    size_t length, const h2_pal_secp192k1_signature_t* signature);

/** Serialized test-only SM2 nonce control. The real pinned upstream signer
 * receives a synthetic ephemeral key through a renamed key-generation call.
 * Production key generation is unchanged. Nonce/private-key inputs must be
 * valid SM2 private scalars. Owned nonce copies are wiped before return. */
h2_pal_result_t h2_wolfcrypt_test_sm2_sign(
    const uint8_t nonce[32], const h2_pal_sm2_private_key_t* private_key,
    const uint8_t digest[32], h2_pal_sm2_signature_t* signature);

#ifdef __cplusplus
}
#endif
#endif
