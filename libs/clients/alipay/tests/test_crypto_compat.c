#include "h2_alipay_crypto_compat.h"

#include "h2_app_test_mem.h"
#include "h2_crypto.h"
#include "h2_wolfcrypt_crypto.h"
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

static void masks(void) {
  static const uint8_t input[] = {0, 1, 2, 3, 4, 5, 6};
  static const uint8_t mask[] = {0x10, 0x20, 0x30};
  static const uint8_t forward[] = {0x10, 0x21, 0x32, 0x13, 0x24, 0x35, 0x16};
  static const uint8_t reverse[] = {0x30, 0x21, 0x12, 0x33, 0x24, 0x15, 0x36};
  uint8_t output[9];
  memset(output, 0xa5, sizeof(output));
  assert(h2_alipay_xor_mask(input, sizeof(input), mask, sizeof(mask), false,
                            output + 1u) == 0);
  assert(h2_crypto_bytes_equal(output + 1u, forward, sizeof(forward)));
  assert(h2_alipay_xor_mask(output + 1u, sizeof(input), mask, sizeof(mask),
                            false, output + 1u) == 0);
  assert(h2_crypto_bytes_equal(output + 1u, input, sizeof(input)));
  assert(h2_alipay_xor_mask(input, sizeof(input), mask, sizeof(mask), true,
                            output + 1u) == 0);
  assert(h2_crypto_bytes_equal(output + 1u, reverse, sizeof(reverse)));
  uint64_t hash = 42u;
  assert(h2_alipay_hash31((const uint8_t*)"abc", 3u, &hash) == 0 &&
         hash == 96354u);
  assert(h2_alipay_hash31(NULL, 1u, &hash) == H2_PAL_ERR_INVALID_ARG &&
         hash == 96354u);
  assert(h2_alipay_hash31(NULL, 0u, &hash) == 0 && hash == 0u);
  assert(output[0] == 0xa5 && output[8] == 0xa5);
  assert(!h2_crypto_bytes_equal(forward, reverse, sizeof(forward)));
  assert(!h2_crypto_bytes_equal(NULL, input, sizeof(input)));
  assert(h2_crypto_bytes_equal(NULL, NULL, 0u));
  assert(h2_alipay_xor_mask(NULL, 0u, mask, sizeof(mask), false, NULL) == 0);
  assert(h2_alipay_xor_mask(input, sizeof(input), NULL, sizeof(mask), false,
                            output) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_xor_mask(input, sizeof(input), mask, 0u, false, output) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_crypto_bytes_equal(output + 1u, reverse, sizeof(reverse)));
}

static void derived_keyset(const h2_pal_crypto_api_t* crypto) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  const h2_alipay_key_material_t material = {(const uint8_t*)"abc", 3u};
  static const uint8_t expected[16] = {0xbb, 0xf5, 0xc1, 0x60, 0x51, 0x4f,
                                       0xb8, 0xd4, 0x1d, 0x65, 0xa0, 0xfb,
                                       0x02, 0x8c, 0xf2, 0xa4};
  h2_alipay_md5_aes_keyset_t* keys = NULL;
  assert(h2_alipay_md5_aes_keyset_open(&memory.api, crypto, &material, 0u,
                                       &keys) == H2_PAL_ERR_INVALID_ARG);
  assert(keys == NULL && memory.live_blocks == 0u);
  assert(h2_alipay_md5_aes_keyset_open(&memory.api, crypto, &material, SIZE_MAX,
                                       &keys) == H2_PAL_ERR_INVALID_ARG);
  memory.fail_at = memory.calls + 1u;
  assert(h2_alipay_md5_aes_keyset_open(&memory.api, crypto, &material, 1u,
                                       &keys) == H2_PAL_ERR_NO_MEMORY);
  assert(keys == NULL && memory.live_blocks == 0u);
  memory.fail_at = 0u;
  const h2_pal_crypto_vtable_t unsupported_vtable = {0};
  const h2_pal_crypto_api_t unsupported = {NULL, &unsupported_vtable};
  assert(h2_alipay_md5_aes_keyset_open(&memory.api, &unsupported, &material, 1u,
                                       &keys) == H2_PAL_ERR_UNSUPPORTED);
  assert(keys == NULL && memory.live_blocks == 0u);
  assert(h2_alipay_md5_aes_keyset_open(&memory.api, crypto, &material, 1u,
                                       &keys) == 0);
  assert(keys != NULL && memory.live_blocks == 1u);
  uint8_t output[18];
  memset(output, 0xa5, sizeof(output));
  size_t length = 0u;
  assert(h2_alipay_md5_aes_keyset_encrypt(keys, 0u, (const uint8_t*)"payload",
                                          7u, output + 1u, 16u, &length) == 0);
  assert(length == 16u && memcmp(output + 1u, expected, 16u) == 0);
  assert(output[0] == 0xa5u && output[17] == 0xa5u);
  memset(output, 0xa5, sizeof(output));
  assert(h2_alipay_md5_aes_keyset_decrypt(keys, 0u, expected, sizeof(expected),
                                          output + 1u, 16u, &length) == 0);
  assert(length == 7u && memcmp(output + 1u, "payload", 7u) == 0);
  assert(output[0] == 0xa5u && output[17] == 0xa5u);
  memset(output, 0xa5, sizeof(output));
  assert(h2_alipay_md5_aes_keyset_encrypt(keys, 1u, (const uint8_t*)"payload",
                                          7u, output, 16u,
                                          &length) == H2_PAL_ERR_INVALID_ARG);
  assert(length == 0u);
  expect_byte(output, sizeof(output), 0xa5u);
  memory.fail_at = memory.calls + 1u;
  assert(h2_alipay_md5_aes_keyset_encrypt(keys, 0u, (const uint8_t*)"payload",
                                          7u, output, 16u,
                                          &length) == H2_PAL_ERR_NO_MEMORY);
  assert(length == 0u && memory.live_blocks == 1u);
  expect_byte(output, sizeof(output), 0xa5u);
  memory.fail_at = 0u;
  h2_alipay_md5_aes_keyset_close(keys);
  h2_alipay_md5_aes_keyset_close(NULL);
  assert(memory.live_blocks == 0u);
}

int main(void) {
  uint32_t random = 1u;
  const h2_wolfcrypt_crypto_config_t config = {&random, entropy};
  assert(h2_wolfcrypt_crypto_init(&config) == 0);
  const h2_pal_crypto_api_t* crypto = h2_wolfcrypt_crypto_api();
  masks();
  derived_keyset(crypto);
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
