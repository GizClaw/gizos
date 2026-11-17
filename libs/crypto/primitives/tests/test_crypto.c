#include "h2_crypto.h"

#include "h2_app_test_mem.h"
#include "h2_wolfcrypt_crypto.h"
#include "h2_wolfcrypt_test_crypto.h"
#include <assert.h>
#include <string.h>

static int entropy(void* user, uint8_t* output, size_t length) {
  uint32_t* state = user;
  for (size_t index = 0u; index < length; ++index) {
    *state = *state * 1664525u + 1013904223u;
    output[index] = (uint8_t)(*state >> 24u);
  }
  return H2_PAL_OK;
}

static void expect_byte(const uint8_t* data, size_t length, uint8_t byte) {
  for (size_t index = 0u; index < length; ++index) assert(data[index] == byte);
}

static void signature_encoding(const h2_pal_crypto_api_t* crypto) {
  h2_pal_p256_signature_t signature = {0};
  signature.bytes[31] = 1u;
  signature.bytes[63] = 2u;
  static const uint8_t expected[] = {0x30, 6, 2, 1, 1, 2, 1, 2};
  uint8_t output[74];
  memset(output, 0xa5, sizeof(output));
  size_t length = 777u;
  assert(h2_crypto_ecdsa_p256_der_encode(&signature, output + 1u, 72u,
                                         &length) == 0);
  assert(length == sizeof(expected));
  assert(memcmp(output + 1u, expected, sizeof(expected)) == 0);
  assert(output[0] == 0xa5u && output[9] == 0xa5u);
  memset(&signature, 0, sizeof(signature));
  signature.bytes[0] = signature.bytes[32] = 0x80u;
  for (size_t capacity = 0u; capacity < 72u; ++capacity) {
    memset(output, 0xa5, sizeof(output));
    assert(h2_crypto_ecdsa_p256_der_encode(&signature, output + 1u, capacity,
                                           &length) == H2_PAL_ERR_NO_SPACE);
    assert(length == 0u);
    expect_byte(output, sizeof(output), 0xa5u);
  }
  assert(h2_crypto_ecdsa_p256_der_encode(&signature, output + 1u, 72u,
                                         &length) == 0);
  assert(length == 72u && output[0] == 0xa5u && output[73] == 0xa5u);
  assert(output[1] == 0x30u && output[2] == 70u && output[3] == 2u &&
         output[4] == 33u && output[5] == 0u && output[6] == 0x80u);
  assert(output[38] == 2u && output[39] == 33u && output[40] == 0u &&
         output[41] == 0x80u);
  memset(output, 0xa5, sizeof(output));
  assert(h2_crypto_ecdsa_p256_der_encode(NULL, output, sizeof(output),
                                         &length) == H2_PAL_ERR_INVALID_ARG);
  assert(length == 0u);
  expect_byte(output, sizeof(output), 0xa5u);
  h2_pal_p256_private_key_t private_key = {0};
  private_key.bytes[31] = 1u;
  h2_pal_p256_keypair_t pair;
  assert(h2_pal_crypto_p256_keypair_from_private(crypto, &private_key, &pair) ==
         0);
  uint8_t point[65], nonce[32] = {0};
  nonce[31] = 1u;
  static const uint8_t message[] = "nonce fixture";
  assert(h2_crypto_p256_public_key_export(crypto, &pair.public_key, point) ==
         0);
  assert(h2_wolfcrypt_test_p256_sign(nonce, &private_key, message,
                                     sizeof(message), &signature) == 0);
  assert(memcmp(signature.bytes, point + 1u, 32u) == 0);
  assert(h2_crypto_ecdsa_p256_sha256_verify(crypto, &pair.public_key, message,
                                            sizeof(message), &signature) == 0);
  memset(nonce, 0x42, sizeof(nonce));
  static const uint8_t fixed_signature[64] = {
      0x3a, 0xd3, 0x86, 0x1a, 0x95, 0x62, 0x13, 0x92, 0x51, 0x6b, 0xb5,
      0x93, 0xef, 0x05, 0x58, 0x3e, 0xd2, 0xe5, 0x86, 0x6f, 0x5c, 0xb6,
      0x26, 0x0a, 0x30, 0x17, 0x23, 0x7f, 0xd8, 0x9b, 0x90, 0xaf, 0xc4,
      0x6a, 0x38, 0xa0, 0x57, 0xb5, 0xe3, 0x41, 0x8c, 0x66, 0x34, 0xb8,
      0xb9, 0x6e, 0x3a, 0xca, 0x7f, 0x80, 0x9f, 0xbf, 0xfa, 0x82, 0xe0,
      0x47, 0x4a, 0x2a, 0x5f, 0xbd, 0x0e, 0x7d, 0x89, 0x86};
  assert(h2_wolfcrypt_test_p256_sign(nonce, &private_key, message,
                                     sizeof(message), &signature) == 0);
  assert(memcmp(signature.bytes, fixed_signature, sizeof(fixed_signature)) ==
         0);
  assert(h2_crypto_ecdsa_p256_sha256_verify(crypto, &pair.public_key, message,
                                            sizeof(message), &signature) == 0);
}

static void masks(void) {
  static const uint8_t input[] = {0, 1, 2, 3, 4, 5, 6};
  static const uint8_t mask[] = {0x10, 0x20, 0x30};
  static const uint8_t forward[] = {0x10, 0x21, 0x32, 0x13, 0x24, 0x35, 0x16};
  static const uint8_t reverse[] = {0x30, 0x21, 0x12, 0x33, 0x24, 0x15, 0x36};
  uint8_t output[9];
  memset(output, 0xa5, sizeof(output));
  assert(h2_crypto_xor_mask(input, sizeof(input), mask, sizeof(mask), false,
                            output + 1u) == 0);
  assert(h2_crypto_bytes_equal(output + 1u, forward, sizeof(forward)));
  assert(h2_crypto_xor_mask(output + 1u, sizeof(input), mask, sizeof(mask),
                            false, output + 1u) == 0);
  assert(h2_crypto_bytes_equal(output + 1u, input, sizeof(input)));
  assert(h2_crypto_xor_mask(input, sizeof(input), mask, sizeof(mask), true,
                            output + 1u) == 0);
  assert(h2_crypto_bytes_equal(output + 1u, reverse, sizeof(reverse)));
  uint64_t hash = 42u;
  assert(h2_crypto_hash31((const uint8_t*)"abc", 3u, &hash) == 0 &&
         hash == 96354u);
  assert(h2_crypto_hash31(NULL, 1u, &hash) == H2_PAL_ERR_INVALID_ARG &&
         hash == 96354u);
  assert(h2_crypto_hash31(NULL, 0u, &hash) == 0 && hash == 0u);
  assert(output[0] == 0xa5 && output[8] == 0xa5);
  assert(!h2_crypto_bytes_equal(forward, reverse, sizeof(forward)));
  assert(!h2_crypto_bytes_equal(NULL, input, sizeof(input)));
  assert(h2_crypto_bytes_equal(NULL, NULL, 0u));
  assert(h2_crypto_xor_mask(NULL, 0u, mask, sizeof(mask), false, NULL) == 0);
  assert(h2_crypto_xor_mask(input, sizeof(input), NULL, sizeof(mask), false,
                            output) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_crypto_xor_mask(input, sizeof(input), mask, 0u, false, output) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_crypto_bytes_equal(output + 1u, reverse, sizeof(reverse)));
}

static void hotp(const h2_pal_crypto_api_t* crypto) {
  static const uint8_t secret[] = "12345678901234567890";
  static const uint32_t expected[] = {755224u, 287082u, 359152u, 969429u,
                                      338314u, 254676u, 287922u, 162583u,
                                      399871u, 520489u};
  for (size_t counter = 0u; counter < 10u; ++counter) {
    uint32_t output = UINT32_MAX;
    assert(h2_crypto_hotp_sha1(crypto, secret, sizeof(secret) - 1u, counter, 6u,
                               &output) == 0);
    assert(output == expected[counter]);
  }
  uint32_t output = UINT32_MAX;
  assert(h2_crypto_hotp_sha1(crypto, secret, sizeof(secret) - 1u, 0u, 8u,
                             &output) == 0 &&
         output == 84755224u);
  assert(h2_crypto_hotp_sha1(NULL, secret, sizeof(secret) - 1u, 0u, 6u,
                             &output) == H2_PAL_ERR_UNSUPPORTED &&
         output == 84755224u);
  assert(h2_crypto_hotp_sha1(crypto, secret, sizeof(secret) - 1u, 0u, 10u,
                             &output) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_crypto_hotp_sha1(crypto, NULL, 0u, 0u, 6u, &output) ==
         H2_PAL_ERR_INVALID_ARG);
}

static int capture_hmac(void* user, const uint8_t* key, size_t key_length,
                        const uint8_t* data, size_t length,
                        uint8_t output[20]) {
  (void)key;
  assert(key_length == 1u && length == 8u);
  assert(memcmp(user, data, length) == 0);
  memset(output, 0, 20u);
  return H2_PAL_ERR_IO;
}

static void counter_width(void) {
  static const uint8_t expected[8] = {0x80u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
  const h2_pal_crypto_vtable_t vtable = {.hmac_sha1 = capture_hmac};
  const h2_pal_crypto_api_t crypto = {(void*)expected, &vtable};
  uint32_t output = 42u;
  assert(h2_crypto_hotp_sha1(&crypto, (const uint8_t*)"K", 1u,
                             UINT64_C(0x8001020304050607), 6u,
                             &output) == H2_PAL_ERR_IO &&
         output == 42u);
}

static void padding(const h2_pal_crypto_api_t* crypto) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  uint8_t input[81], key[32], iv[16] = {0};
  uint8_t encrypted[98], decrypted[98], raw[96];
  for (size_t index = 0u; index < sizeof(input); ++index)
    input[index] = (uint8_t)index;
  memset(key, 0x42, sizeof(key));
  for (size_t key_length = 16u; key_length <= 32u; key_length += 8u) {
    for (size_t size = 0u; size <= sizeof(input); ++size) {
      size_t cipher_length = 777u, plain_length = 777u;
      memset(encrypted, 0xa5, sizeof(encrypted));
      memset(decrypted, 0xa5, sizeof(decrypted));
      assert(h2_crypto_aes_cbc_pkcs7_encrypt(
                 crypto, &memory.api, key, key_length, iv,
                 size != 0u ? input : NULL, size, encrypted + 1u, 96u,
                 &cipher_length) == 0);
      assert(cipher_length == size + 16u - size % 16u);
      assert(encrypted[0] == 0xa5u && encrypted[cipher_length + 1u] == 0xa5u);
      h2_pal_crypto_buf_t buffer = {.data = raw, .cap = sizeof(raw)};
      assert(h2_crypto_aes_cbc(crypto, H2_PAL_CRYPTO_CIPHER_DECRYPT, key,
                               key_length, iv, encrypted + 1u, cipher_length,
                               &buffer) == 0);
      assert(buffer.len == cipher_length && memcmp(raw, input, size) == 0);
      expect_byte(raw + size, cipher_length - size,
                  (uint8_t)(cipher_length - size));
      assert(h2_crypto_aes_cbc_pkcs7_decrypt(
                 crypto, key, key_length, iv, encrypted + 1u, cipher_length,
                 decrypted + 1u, 96u, &plain_length) == 0);
      assert(plain_length == size && memcmp(decrypted + 1u, input, size) == 0);
      assert(decrypted[0] == 0xa5u && decrypted[cipher_length + 1u] == 0xa5u);
      memset(decrypted, 0xa5, sizeof(decrypted));
      assert(h2_crypto_aes_cbc_pkcs7_decrypt(
                 crypto, key, key_length, iv, encrypted + 1u, cipher_length,
                 decrypted + 1u, cipher_length - 1u,
                 &plain_length) == H2_PAL_ERR_NO_SPACE);
      assert(plain_length == 0u);
      expect_byte(decrypted, sizeof(decrypted), 0xa5u);
    }
  }
  for (unsigned pad = 0u; pad <= 17u; ++pad) {
    memset(raw, 0, 16u);
    raw[15] = (uint8_t)pad;
    h2_pal_crypto_buf_t buffer = {.data = encrypted, .cap = 16u};
    assert(h2_crypto_aes_cbc(crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT, key, 16u, iv,
                             raw, 16u, &buffer) == 0);
    memset(decrypted, 0xa5, sizeof(decrypted));
    size_t length = 777u;
    int rc = h2_crypto_aes_cbc_pkcs7_decrypt(crypto, key, 16u, iv, encrypted,
                                             16u, decrypted + 1u, 16u, &length);
    if (pad == 1u)
      assert(rc == 0 && length == 15u);
    else {
      assert(rc == H2_PAL_ERR_FORMAT && length == 0u);
      expect_byte(decrypted + 1u, 16u, 0u);
    }
    assert(decrypted[0] == 0xa5u && decrypted[17] == 0xa5u);
  }
  memset(encrypted, 0xa5, sizeof(encrypted));
  memory.fail_at = memory.calls + 1u;
  size_t length = 777u;
  assert(h2_crypto_aes_cbc_pkcs7_encrypt(crypto, &memory.api, key, 16u, iv,
                                         input, 1u, encrypted, 16u,
                                         &length) == H2_PAL_ERR_NO_MEMORY);
  assert(length == 0u && memory.live_blocks == 0u);
  expect_byte(encrypted, sizeof(encrypted), 0xa5u);
  assert(h2_crypto_aes_cbc_pkcs7_decrypt(NULL, key, 16u, iv, input, 16u,
                                         decrypted, 16u,
                                         &length) == H2_PAL_ERR_UNSUPPORTED);
  expect_byte(decrypted, 16u, 0u);
  assert(h2_crypto_aes_cbc_pkcs7_decrypt(crypto, key, 16u, iv, input, 15u,
                                         decrypted, 16u,
                                         &length) == H2_PAL_ERR_FORMAT);
  assert(h2_crypto_aes_cbc_pkcs7_encrypt(
             crypto, &memory.api, key, 16u, iv, input, SIZE_MAX, encrypted,
             sizeof(encrypted), &length) == H2_PAL_ERR_INVALID_ARG);
  assert(memory.live_blocks == 0u);
}

static int partial_cipher(void* user, h2_pal_crypto_cipher_direction_t mode,
                          const uint8_t* key, size_t key_length,
                          const uint8_t iv[16], const uint8_t* data,
                          size_t length, h2_pal_crypto_buf_t* output) {
  (void)user;
  (void)mode;
  (void)key;
  (void)key_length;
  (void)iv;
  (void)data;
  assert(length == 16u && output->cap >= 16u);
  memset(output->data, 0x42, 3u);
  output->len = 3u;
  return H2_PAL_ERR_IO;
}

static void partial_failure(void) {
  const h2_pal_crypto_vtable_t vtable = {.aes_cbc = partial_cipher};
  const h2_pal_crypto_api_t crypto = {NULL, &vtable};
  uint8_t key[16] = {0}, iv[16] = {0}, input[16] = {0}, output[18];
  memset(output, 0xa5, sizeof(output));
  size_t length = 777u;
  assert(h2_crypto_aes_cbc_pkcs7_decrypt(&crypto, key, 16u, iv, input, 16u,
                                         output + 1u, 16u,
                                         &length) == H2_PAL_ERR_IO);
  assert(length == 0u && output[0] == 0xa5u && output[17] == 0xa5u);
  expect_byte(output + 1u, 16u, 0u);
}

static void points(const h2_pal_crypto_api_t* crypto) {
  h2_pal_p256_keypair_t first, second;
  assert(h2_crypto_p256_keypair_generate(crypto, &first) == 0);
  assert(h2_crypto_p256_keypair_generate(crypto, &second) == 0);
  uint8_t expanded[65], secret[32], other[32];
  h2_pal_p256_public_key_t imported, retained;
  assert(h2_crypto_p256_public_key_export(crypto, &first.public_key,
                                          expanded) == 0);
  assert(h2_crypto_p256_public_key_import(crypto, expanded, &imported) == 0);
  assert(memcmp(&imported, &first.public_key, sizeof(imported)) == 0);
  retained = imported;
  expanded[64] ^= 2u;
  assert(h2_crypto_p256_public_key_import(crypto, expanded, &imported) ==
         H2_PAL_ERR_FORMAT);
  assert(memcmp(&imported, &retained, sizeof(imported)) == 0);
  expanded[64] ^= 2u;
  expanded[0] = 3u;
  assert(h2_crypto_p256_public_key_import(crypto, expanded, &imported) ==
         H2_PAL_ERR_FORMAT);
  expanded[0] = 4u;
  assert(h2_crypto_p256_public_key_import(NULL, expanded, &imported) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_crypto_p256_shared_secret(crypto, &first.private_key,
                                      &second.public_key, secret) == 0);
  assert(h2_crypto_p256_shared_secret(crypto, &second.private_key,
                                      &first.public_key, other) == 0);
  assert(memcmp(secret, other, sizeof(secret)) == 0);
  static const uint8_t message[] = "synthetic signature verification";
  h2_pal_p256_signature_t signature;
  assert(h2_pal_crypto_ecdsa_p256_sha256_sign(crypto, &first.private_key,
                                              message, sizeof(message),
                                              &signature) == 0);
  assert(h2_crypto_ecdsa_p256_sha256_verify(crypto, &first.public_key, message,
                                            sizeof(message), &signature) == 0);
  assert(h2_crypto_ecdsa_p256_sha256_verify(crypto, &second.public_key, message,
                                            sizeof(message),
                                            &signature) == H2_PAL_ERR_FORMAT);
  signature.bytes[63] ^= 1u;
  assert(h2_crypto_ecdsa_p256_sha256_verify(crypto, &first.public_key, message,
                                            sizeof(message),
                                            &signature) == H2_PAL_ERR_FORMAT);
  assert(h2_crypto_ecdsa_p256_sha256_verify(NULL, &first.public_key, message,
                                            sizeof(message), &signature) ==
         H2_PAL_ERR_UNSUPPORTED);
}

static void derived_keyset(const h2_pal_crypto_api_t* crypto) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  const h2_crypto_key_material_t material = {(const uint8_t*)"abc", 3u};
  static const uint8_t expected[16] = {0xbb, 0xf5, 0xc1, 0x60, 0x51, 0x4f,
                                       0xb8, 0xd4, 0x1d, 0x65, 0xa0, 0xfb,
                                       0x02, 0x8c, 0xf2, 0xa4};
  h2_crypto_md5_aes_keyset_t* keys = NULL;
  assert(h2_crypto_md5_aes_keyset_open(&memory.api, crypto, &material, 0u,
                                       &keys) == H2_PAL_ERR_INVALID_ARG);
  assert(keys == NULL && memory.live_blocks == 0u);
  assert(h2_crypto_md5_aes_keyset_open(&memory.api, crypto, &material, SIZE_MAX,
                                       &keys) == H2_PAL_ERR_INVALID_ARG);
  memory.fail_at = memory.calls + 1u;
  assert(h2_crypto_md5_aes_keyset_open(&memory.api, crypto, &material, 1u,
                                       &keys) == H2_PAL_ERR_NO_MEMORY);
  assert(keys == NULL && memory.live_blocks == 0u);
  memory.fail_at = 0u;
  const h2_pal_crypto_vtable_t unsupported_vtable = {0};
  const h2_pal_crypto_api_t unsupported = {NULL, &unsupported_vtable};
  assert(h2_crypto_md5_aes_keyset_open(&memory.api, &unsupported, &material, 1u,
                                       &keys) == H2_PAL_ERR_UNSUPPORTED);
  assert(keys == NULL && memory.live_blocks == 0u);
  assert(h2_crypto_md5_aes_keyset_open(&memory.api, crypto, &material, 1u,
                                       &keys) == 0);
  assert(keys != NULL && memory.live_blocks == 1u);
  uint8_t output[18];
  memset(output, 0xa5, sizeof(output));
  size_t length = 0u;
  assert(h2_crypto_md5_aes_keyset_encrypt(keys, 0u, (const uint8_t*)"payload",
                                          7u, output + 1u, 16u, &length) == 0);
  assert(length == 16u && memcmp(output + 1u, expected, 16u) == 0);
  assert(output[0] == 0xa5u && output[17] == 0xa5u);
  memset(output, 0xa5, sizeof(output));
  assert(h2_crypto_md5_aes_keyset_decrypt(keys, 0u, expected, sizeof(expected),
                                          output + 1u, 16u, &length) == 0);
  assert(length == 7u && memcmp(output + 1u, "payload", 7u) == 0);
  assert(output[0] == 0xa5u && output[17] == 0xa5u);
  memset(output, 0xa5, sizeof(output));
  assert(h2_crypto_md5_aes_keyset_encrypt(keys, 1u, (const uint8_t*)"payload",
                                          7u, output, 16u,
                                          &length) == H2_PAL_ERR_INVALID_ARG);
  assert(length == 0u);
  expect_byte(output, sizeof(output), 0xa5u);
  memory.fail_at = memory.calls + 1u;
  assert(h2_crypto_md5_aes_keyset_encrypt(keys, 0u, (const uint8_t*)"payload",
                                          7u, output, 16u,
                                          &length) == H2_PAL_ERR_NO_MEMORY);
  assert(length == 0u && memory.live_blocks == 1u);
  expect_byte(output, sizeof(output), 0xa5u);
  memory.fail_at = 0u;
  h2_crypto_md5_aes_keyset_close(keys);
  h2_crypto_md5_aes_keyset_close(NULL);
  assert(memory.live_blocks == 0u);
}

static void legacy_signature(const h2_pal_crypto_api_t* crypto) {
  h2_pal_secp192k1_private_key_t private_key = {{0}};
  private_key.bytes[23] = 1u;
  h2_pal_secp192k1_signature_t signature;
  uint8_t nonce[24];
  memset(nonce, 0x42, sizeof(nonce));
  static const uint8_t expected[48] = {
      0x63, 0x1b, 0xaf, 0x86, 0x66, 0x7f, 0xb6, 0x59, 0x97, 0x25, 0x36, 0x98,
      0x00, 0x45, 0x97, 0xa8, 0x5b, 0x3b, 0x30, 0xe9, 0xcf, 0x3e, 0x1b, 0xea,
      0x33, 0x85, 0x92, 0x51, 0xf9, 0x2e, 0x58, 0x13, 0x35, 0xa0, 0x74, 0xbd,
      0x0e, 0xfd, 0xa0, 0xeb, 0xc9, 0x8b, 0x66, 0x88, 0x4c, 0xee, 0xb8, 0x8f};
  const uint8_t message[] = "abc";
  assert(h2_wolfcrypt_test_secp192k1_sign(nonce, &private_key, message, 3u,
                                          &signature) == 0);
  assert(memcmp(signature.bytes, expected, sizeof(expected)) == 0);
  uint8_t encoded[58];
  size_t length = 0u;
  memset(encoded, 0xa5, sizeof(encoded));
  assert(h2_crypto_ecdsa_der_encode(signature.bytes, 24u, encoded + 1u, 56u,
                                    &length) == 0);
  assert(length == 54u && encoded[0] == 0xa5u && encoded[55] == 0xa5u &&
         encoded[1] == 0x30u && encoded[2] == 52u && encoded[3] == 2u &&
         encoded[4] == 24u && memcmp(encoded + 5u, expected, 24u) == 0 &&
         encoded[29] == 2u && encoded[30] == 24u &&
         memcmp(encoded + 31u, expected + 24u, 24u) == 0);
  for (size_t capacity = 0u; capacity < length; ++capacity) {
    memset(encoded, 0xa5, sizeof(encoded));
    size_t written = 999u;
    assert(h2_crypto_ecdsa_der_encode(signature.bytes, 24u, encoded + 1u,
                                      capacity,
                                      &written) == H2_PAL_ERR_NO_SPACE);
    assert(written == 0u);
    expect_byte(encoded, sizeof(encoded), 0xa5u);
  }
  assert(h2_crypto_ecdsa_der_encode(signature.bytes, 0u, encoded, 56u,
                                    &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_crypto_ecdsa_der_encode(signature.bytes, 33u, encoded, 56u,
                                    &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_crypto_ecdsa_secp192k1_sha1_sign(crypto, &private_key, message, 3u,
                                             &signature) == 0);
  assert(h2_wolfcrypt_test_secp192k1_verify(&private_key, message, 3u,
                                            &signature) == 0);
  signature.bytes[47] ^= 1u;
  assert(h2_wolfcrypt_test_secp192k1_verify(&private_key, message, 3u,
                                            &signature) != 0);
  assert(h2_crypto_ecdsa_secp192k1_sha1_sign(NULL, &private_key, message, 3u,
                                             &signature) ==
         H2_PAL_ERR_UNSUPPORTED);
  expect_byte(signature.bytes, sizeof(signature.bytes), 0u);
  memset(&private_key, 0, sizeof(private_key));
  assert(h2_crypto_ecdsa_secp192k1_sha1_sign(crypto, &private_key, message, 3u,
                                             &signature) == H2_PAL_ERR_FORMAT);
  expect_byte(signature.bytes, sizeof(signature.bytes), 0u);
  memset(nonce, 0, sizeof(nonce));
  assert(h2_wolfcrypt_test_secp192k1_sign(nonce, &private_key, message, 3u,
                                          &signature) == H2_PAL_ERR_FORMAT);
}

static void sm2_signature(const h2_pal_crypto_api_t* crypto) {
  h2_pal_sm2_private_key_t private_key = {{0}};
  private_key.bytes[31] = 1u;
  h2_pal_sm2_public_key_t public_key;
  assert(h2_pal_crypto_sm2_public_key_from_private(crypto, &private_key,
                                                   &public_key) == 0);
  static const uint8_t generator[65] = {
      0x04, 0x32, 0xc4, 0xae, 0x2c, 0x1f, 0x19, 0x81, 0x19, 0x5f, 0x99,
      0x04, 0x46, 0x6a, 0x39, 0xc9, 0x94, 0x8f, 0xe3, 0x0b, 0xbf, 0xf2,
      0x66, 0x0b, 0xe1, 0x71, 0x5a, 0x45, 0x89, 0x33, 0x4c, 0x74, 0xc7,
      0xbc, 0x37, 0x36, 0xa2, 0xf4, 0xf6, 0x77, 0x9c, 0x59, 0xbd, 0xce,
      0xe3, 0x6b, 0x69, 0x21, 0x53, 0xd0, 0xa9, 0x87, 0x7c, 0xc6, 0x2a,
      0x47, 0x40, 0x02, 0xdf, 0x32, 0xe5, 0x21, 0x39, 0xf0, 0xa0};
  assert(memcmp(public_key.bytes, generator, sizeof(generator)) == 0);
  static const uint8_t identity[] = "1234567812345678";
  static const uint8_t message[] = "abc";
  static const h2_pal_sm2_signature_t known = {
      {0x0b, 0x6c, 0x19, 0xd4, 0xf9, 0xfe, 0x5a, 0x23, 0x6f, 0x5b, 0x5e,
       0xdd, 0x82, 0x1f, 0xa2, 0xf0, 0xa9, 0xc6, 0x3f, 0x5c, 0x4a, 0xcb,
       0x39, 0x19, 0xcc, 0x7f, 0x28, 0xdf, 0x22, 0xb4, 0x76, 0x56, 0x1b,
       0x6b, 0x14, 0x36, 0xa4, 0x21, 0xf4, 0x0f, 0x69, 0x73, 0x71, 0xb2,
       0x60, 0x11, 0x4f, 0xa8, 0xcc, 0x3e, 0x01, 0x72, 0xfb, 0xbb, 0x84,
       0x94, 0x3a, 0xe1, 0x8c, 0xb1, 0x8f, 0xc6, 0xe5, 0xf6}};
  assert(h2_crypto_sm2_verify(crypto, &public_key, identity, 16u, message, 3u,
                              &known) == 0);
  h2_pal_sm2_signature_t signature;
  uint8_t nonce[32];
  memset(nonce, 0x42, sizeof(nonce));
  static const uint8_t digest[32] = {
      0xe2, 0x63, 0x1e, 0x76, 0xcf, 0x38, 0x54, 0x6a, 0xa6, 0xcf, 0x0f,
      0xba, 0x91, 0xa4, 0x89, 0x4f, 0xec, 0x7f, 0xf6, 0x5c, 0x7c, 0xdb,
      0x9d, 0xa0, 0x24, 0x64, 0x17, 0xfc, 0xb1, 0x86, 0x7b, 0x95};
  assert(h2_wolfcrypt_test_sm2_sign(nonce, &private_key, digest, &signature) ==
         0);
  assert(memcmp(&signature, &known, sizeof(known)) == 0);
  assert(h2_crypto_sm2_sign(crypto, &private_key, identity, 16u, message, 3u,
                            &signature) == 0);
  assert(h2_crypto_sm2_verify(crypto, &public_key, identity, 16u, message, 3u,
                              &signature) == 0);
  assert(h2_crypto_sm2_verify(crypto, &public_key, identity, 15u, message, 3u,
                              &signature) == H2_PAL_ERR_FORMAT);
  signature.bytes[63] ^= 1u;
  assert(h2_crypto_sm2_verify(crypto, &public_key, identity, 16u, message, 3u,
                              &signature) == H2_PAL_ERR_FORMAT);
  assert(h2_crypto_sm2_sign(crypto, &private_key, identity, 8192u, message, 3u,
                            &signature) == H2_PAL_ERR_INVALID_ARG);
  expect_byte(signature.bytes, sizeof(signature.bytes), 0u);
  assert(h2_crypto_sm2_sign(crypto, &private_key, NULL, 0u, NULL, 0u,
                            &signature) == 0);
  assert(h2_crypto_sm2_verify(crypto, &public_key, NULL, 0u, NULL, 0u,
                              &signature) == 0);
  assert(h2_crypto_sm2_sign(NULL, &private_key, identity, 16u, message, 3u,
                            &signature) == H2_PAL_ERR_UNSUPPORTED);
  expect_byte(signature.bytes, sizeof(signature.bytes), 0u);
  memset(&private_key, 0, sizeof(private_key));
  assert(h2_crypto_sm2_sign(crypto, &private_key, identity, 16u, message, 3u,
                            &signature) == H2_PAL_ERR_FORMAT);
  expect_byte(signature.bytes, sizeof(signature.bytes), 0u);
}

static void legacy_des(const h2_pal_crypto_api_t* crypto) {
  static const uint8_t key_part[8] = {0x13, 0x34, 0x57, 0x79,
                                      0x9b, 0xbc, 0xdf, 0xf1};
  static const uint8_t input[8] = {0x01, 0x23, 0x45, 0x67,
                                   0x89, 0xab, 0xcd, 0xef};
  static const uint8_t expected[8] = {0x85, 0xe8, 0x13, 0x54,
                                      0x0f, 0x0a, 0xb4, 0x05};
  uint8_t key[24], iv[8] = {0}, output[10];
  for (size_t index = 0u; index < 3u; ++index)
    memcpy(key + index * 8u, key_part, sizeof(key_part));
  for (size_t key_length = 8u; key_length <= 24u; key_length += 8u) {
    memset(output, 0xa5, sizeof(output));
    h2_pal_crypto_buf_t encrypted = {output + 1u, 999u, 8u};
    assert(h2_crypto_des_cbc(crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT, key,
                             key_length, iv, input, sizeof(input),
                             &encrypted) == 0);
    assert(encrypted.len == 8u && output[0] == 0xa5u && output[9] == 0xa5u &&
           memcmp(output + 1u, expected, 8u) == 0);
    assert(h2_crypto_des_cbc(crypto, H2_PAL_CRYPTO_CIPHER_DECRYPT, key,
                             key_length, iv, output + 1u, 8u, &encrypted) == 0);
    assert(memcmp(output + 1u, input, 8u) == 0);
    expect_byte(iv, sizeof(iv), 0u);
    memset(output, 0xa5, sizeof(output));
    encrypted.cap = 7u;
    assert(h2_crypto_des_cbc(crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT, key,
                             key_length, iv, input, 8u,
                             &encrypted) == H2_PAL_ERR_NO_SPACE);
    assert(encrypted.len == 0u);
    expect_byte(output, sizeof(output), 0xa5u);
  }
  uint8_t tag[8];
  memset(tag, 0xa5, sizeof(tag));
  assert(h2_crypto_iso9797_alg3_mac(NULL, key, input, sizeof(input), tag) ==
         H2_PAL_ERR_UNSUPPORTED);
  expect_byte(tag, sizeof(tag), 0u);
  assert(h2_crypto_iso9797_alg3_mac(crypto, key, NULL, 1u, tag) ==
         H2_PAL_ERR_INVALID_ARG);
  expect_byte(tag, sizeof(tag), 0u);
  assert(h2_crypto_iso9797_alg3_mac(crypto, key, NULL, 0u, tag) == 0);
  assert(h2_crypto_iso9797_alg3_mac(crypto, NULL, input, 8u, tag) ==
         H2_PAL_ERR_INVALID_ARG);
  expect_byte(tag, sizeof(tag), 0u);
}

static void legacy_hashes(const h2_pal_crypto_api_t* crypto) {
  static const uint8_t sha1_expected[20] = {
      0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
      0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
  static const uint8_t hmac_expected[16] = {0x92, 0x94, 0x72, 0x7a, 0x36, 0x38,
                                            0xbb, 0x1c, 0x13, 0xf4, 0x8e, 0xf8,
                                            0x15, 0x8b, 0xfc, 0x9d};
  uint8_t output[20], key[16];
  assert(h2_crypto_sha1(crypto, (const uint8_t*)"abc", 3u, output) == 0);
  assert(memcmp(output, sha1_expected, 20u) == 0);
  memset(key, 0x0b, sizeof(key));
  assert(h2_crypto_hmac_md5(crypto, key, sizeof(key),
                            (const uint8_t*)"Hi There", 8u, output) == 0);
  assert(memcmp(output, hmac_expected, 16u) == 0);
  assert(h2_crypto_sha1(NULL, NULL, 0u, output) == H2_PAL_ERR_UNSUPPORTED);
  expect_byte(output, 20u, 0u);
  assert(h2_crypto_sha1(crypto, NULL, 1u, output) == H2_PAL_ERR_INVALID_ARG);
  expect_byte(output, 20u, 0u);
  assert(h2_crypto_hmac_md5(NULL, NULL, 0u, NULL, 0u, output) ==
         H2_PAL_ERR_UNSUPPORTED);
  expect_byte(output, 16u, 0u);
}

int main(void) {
  uint32_t random = 1u;
  const h2_wolfcrypt_crypto_config_t config = {&random, entropy};
  assert(h2_wolfcrypt_crypto_init(&config) == 0);
  const h2_pal_crypto_api_t* crypto = h2_wolfcrypt_crypto_api();
  hotp(crypto);
  masks();
  counter_width();
  padding(crypto);
  partial_failure();
  points(crypto);
  derived_keyset(crypto);
  signature_encoding(crypto);
  legacy_signature(crypto);
  sm2_signature(crypto);
  legacy_des(crypto);
  legacy_hashes(crypto);
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
