#include "h2_wolfcrypt_test_crypto.h"

#include "h2_wolfcrypt_crypto.h"
#include "../src/h2_wolfcrypt_crypto_internal.h"
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/integer.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sm2.h>

static uint8_t sm2_nonce[32];

extern int h2_wolfcrypt_fixture_sm2_sign_hash_ex(const byte* hash,
                                                 word32 length, WC_RNG* rng,
                                                 ecc_key* key, mp_int* first,
                                                 mp_int* second);

int h2_wolfcrypt_test_sm2_make_key_ex2(WC_RNG* rng, int size, ecc_key* key,
                                       int curve, int flags) {
  if (rng == NULL || key == NULL || size != 32 || curve != ECC_SM2P256V1 ||
      flags != 0)
    return BAD_FUNC_ARG;
  int rc = wc_ecc_import_private_key_ex(sm2_nonce, sizeof(sm2_nonce), NULL, 0u,
                                        key, ECC_SM2P256V1);
  if (rc == 0) rc = wc_ecc_make_pub_ex(key, NULL, rng);
  return rc;
}

h2_pal_result_t h2_wolfcrypt_test_p256_sign(
    const uint8_t nonce[32], const h2_pal_p256_private_key_t* private_key,
    const uint8_t* message, size_t length, h2_pal_p256_signature_t* signature) {
  if (signature == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(signature, 0, sizeof(*signature));
  if (nonce == NULL || private_key == NULL || length > UINT32_MAX ||
      (message == NULL && length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_p256_private_key_t nonce_key;
  h2_pal_p256_keypair_t nonce_pair;
  memcpy(nonce_key.bytes, nonce, sizeof(nonce_key.bytes));
  int valid = h2_pal_crypto_p256_keypair_from_private(h2_wolfcrypt_crypto_api(),
                                                      &nonce_key, &nonce_pair);
  h2_wolfcrypt_secure_zero(&nonce_key, sizeof(nonce_key));
  h2_wolfcrypt_secure_zero(&nonce_pair, sizeof(nonce_pair));
  if (valid != 0) return valid;
  uint8_t digest[32];
  ecc_key key;
  WC_RNG rng;
  mp_int first, second;
  bool key_ready = false, rng_ready = false, integers_ready = false;
  int rc = wc_Sha256Hash(message, (word32)length, digest);
  if (rc == 0) {
    rc = wc_ecc_init(&key);
    key_ready = rc == 0;
  }
  if (rc == 0) {
    rc = wc_InitRng(&rng);
    rng_ready = rc == 0;
  }
  if (rc == 0) {
    rc = mp_init_multi(&first, &second, NULL, NULL, NULL, NULL);
    integers_ready = rc == 0;
  }
  if (rc == 0) {
    rc = wc_ecc_import_private_key_ex(private_key->bytes, 32u, NULL, 0u, &key,
                                      ECC_SECP256R1);
  }
  if (rc == 0) rc = wc_ecc_sign_set_k(nonce, 32u, &key);
  if (rc == 0) {
    rc = wc_ecc_sign_hash_ex(digest, sizeof(digest), &rng, &key, &first,
                             &second);
  }
  if (rc == 0) rc = mp_to_unsigned_bin_len(&first, signature->bytes, 32u);
  if (rc == 0)
    rc = mp_to_unsigned_bin_len(&second, signature->bytes + 32u, 32u);
  if (integers_ready) {
    mp_clear(&first);
    mp_clear(&second);
  }
  if (rng_ready) (void)wc_FreeRng(&rng);
  if (key_ready) wc_ecc_free(&key);
  h2_wolfcrypt_secure_zero(digest, sizeof(digest));
  if (rc != 0) memset(signature, 0, sizeof(*signature));
  return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t secp192k1_fixture(
    const uint8_t* nonce, const h2_pal_secp192k1_private_key_t* private_key,
    const uint8_t* message, size_t length,
    h2_pal_secp192k1_signature_t* signature) {
  if (private_key == NULL || length > UINT32_MAX || signature == NULL ||
      (message == NULL && length != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  static const uint8_t order[24] = {
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
      0x26, 0xf2, 0xfc, 0x17, 0x0f, 0x69, 0x46, 0x6a, 0x74, 0xde, 0xfd, 0x8d};
  uint8_t combined = 0u;
  for (size_t index = 0u; nonce != NULL && index < 24u; ++index)
    combined |= nonce[index];
  if (nonce != NULL && (combined == 0u || memcmp(nonce, order, 24u) >= 0))
    return H2_PAL_ERR_FORMAT;
  uint8_t digest[20];
  ecc_key key;
  WC_RNG rng;
  mp_int first, second;
  bool key_ready = false, rng_ready = false, integers_ready = false;
  int rc = wc_ShaHash(message, (word32)length, digest);
  if (rc == 0) {
    rc = wc_ecc_init(&key);
    key_ready = rc == 0;
  }
  if (rc == 0) {
    rc = wc_InitRng(&rng);
    rng_ready = rc == 0;
  }
  if (rc == 0) {
    rc = mp_init_multi(&first, &second, NULL, NULL, NULL, NULL);
    integers_ready = rc == 0;
  }
  if (rc == 0)
    rc = wc_ecc_import_private_key_ex(private_key->bytes, 24u, NULL, 0u, &key,
                                      ECC_SECP192K1);
  if (rc == 0 && nonce != NULL) rc = wc_ecc_sign_set_k(nonce, 24u, &key);
  if (rc == 0 && nonce != NULL)
    rc = wc_ecc_sign_hash_ex(digest, sizeof(digest), &rng, &key, &first,
                             &second);
  if (rc == 0 && nonce != NULL)
    rc = mp_to_unsigned_bin_len(&first, signature->bytes, 24u);
  if (rc == 0 && nonce != NULL)
    rc = mp_to_unsigned_bin_len(&second, signature->bytes + 24u, 24u);
  if (rc == 0 && nonce == NULL)
    rc = mp_read_unsigned_bin(&first, signature->bytes, 24u);
  if (rc == 0 && nonce == NULL)
    rc = mp_read_unsigned_bin(&second, signature->bytes + 24u, 24u);
  if (rc == 0) rc = wc_ecc_make_pub_ex(&key, NULL, &rng);
  int verified = 0;
  if (rc == 0)
    rc = wc_ecc_verify_hash_ex(&first, &second, digest, sizeof(digest),
                               &verified, &key);
  if (integers_ready) {
    mp_clear(&first);
    mp_clear(&second);
  }
  if (rng_ready) (void)wc_FreeRng(&rng);
  if (key_ready) wc_ecc_free(&key);
  h2_wolfcrypt_secure_zero(digest, sizeof(digest));
  return rc == 0 && verified == 1 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_wolfcrypt_test_secp192k1_sign(
    const uint8_t nonce[24], const h2_pal_secp192k1_private_key_t* private_key,
    const uint8_t* message, size_t length,
    h2_pal_secp192k1_signature_t* signature) {
  if (signature == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(signature, 0, sizeof(*signature));
  if (nonce == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = secp192k1_fixture(nonce, private_key, message, length, signature);
  if (rc != 0) memset(signature, 0, sizeof(*signature));
  return rc;
}

h2_pal_result_t h2_wolfcrypt_test_secp192k1_verify(
    const h2_pal_secp192k1_private_key_t* private_key, const uint8_t* message,
    size_t length, const h2_pal_secp192k1_signature_t* signature) {
  if (signature == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_secp192k1_signature_t copy = *signature;
  int rc = secp192k1_fixture(NULL, private_key, message, length, &copy);
  h2_wolfcrypt_secure_zero(&copy, sizeof(copy));
  return rc;
}

h2_pal_result_t h2_wolfcrypt_test_sm2_sign(
    const uint8_t nonce[32], const h2_pal_sm2_private_key_t* private_key,
    const uint8_t digest[32], h2_pal_sm2_signature_t* signature) {
  if (signature == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(signature, 0, sizeof(*signature));
  if (nonce == NULL || private_key == NULL || digest == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_sm2_private_key_t nonce_key;
  h2_pal_sm2_public_key_t public_key;
  memcpy(nonce_key.bytes, nonce, sizeof(nonce_key.bytes));
  int rc = h2_pal_crypto_sm2_public_key_from_private(h2_wolfcrypt_crypto_api(),
                                                     &nonce_key, &public_key);
  h2_wolfcrypt_secure_zero(&nonce_key, sizeof(nonce_key));
  if (rc != 0) return rc;
  rc = h2_pal_crypto_sm2_public_key_from_private(h2_wolfcrypt_crypto_api(),
                                                 private_key, &public_key);
  if (rc != 0) return rc;
  memcpy(sm2_nonce, nonce, sizeof(sm2_nonce));
  ecc_key key;
  WC_RNG rng;
  mp_int first, second;
  bool key_ready = false, rng_ready = false, integers_ready = false;
  rc = wc_ecc_init(&key);
  key_ready = rc == 0;
  if (rc == 0) {
    rc = wc_InitRng(&rng);
    rng_ready = rc == 0;
  }
  if (rc == 0) {
    rc = mp_init_multi(&first, &second, NULL, NULL, NULL, NULL);
    integers_ready = rc == 0;
  }
  if (rc == 0)
    rc = wc_ecc_import_private_key_ex(private_key->bytes, 32u, NULL, 0u, &key,
                                      ECC_SM2P256V1);
  if (rc == 0)
    rc = h2_wolfcrypt_fixture_sm2_sign_hash_ex(digest, 32u, &rng, &key, &first,
                                               &second);
  if (rc == 0) rc = mp_to_unsigned_bin_len(&first, signature->bytes, 32u);
  if (rc == 0)
    rc = mp_to_unsigned_bin_len(&second, signature->bytes + 32u, 32u);
  if (integers_ready) {
    mp_clear(&first);
    mp_clear(&second);
  }
  if (rng_ready) (void)wc_FreeRng(&rng);
  if (key_ready) wc_ecc_free(&key);
  h2_wolfcrypt_secure_zero(sm2_nonce, sizeof(sm2_nonce));
  if (rc == 0)
    rc = h2_pal_crypto_sm2_verify_digest(h2_wolfcrypt_crypto_api(), &public_key,
                                         digest, signature);
  h2_wolfcrypt_secure_zero(&public_key, sizeof(public_key));
  if (rc != 0) memset(signature, 0, sizeof(*signature));
  return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}
