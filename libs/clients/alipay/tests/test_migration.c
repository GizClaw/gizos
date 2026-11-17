#include "h2_alipay_context.h"

#include "h2_alipay_api.h"
#include "h2_crypto.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"
#include <assert.h>
#include <string.h>

typedef struct fixture {
  h2_app_test_mem_t memory;
  h2_app_test_pref_t pref;
  h2_app_test_sync_t sync;
  h2_app_test_time_t time;
  h2_pal_http_api_t http;
  h2_alipay_client_config_t config;
} fixture_t;

static int entropy(void* user, uint8_t* output, size_t length) {
  uint32_t* state = user;
  for (size_t index = 0u; index < length; ++index) {
    *state = *state * 1664525u + 1013904223u;
    output[index] = (uint8_t)(*state >> 24u);
  }
  return H2_PAL_OK;
}

static int unexpected_http(void* user, const h2_pal_http_request_t* request,
                           h2_pal_http_response_t* response) {
  (void)user;
  (void)request;
  (void)response;
  assert(false);
  return H2_PAL_ERR_IO;
}

static void initialize(fixture_t* fixture) {
  memset(fixture, 0, sizeof(*fixture));
  h2_app_test_mem_init(&fixture->memory, NULL);
  h2_app_test_sync_init(&fixture->sync);
  h2_app_test_time_init(&fixture->time, 0u);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  assert(h2_app_test_pref_init(&fixture->pref, &fixture->memory.api) == 0);
  static const h2_pal_http_vtable_t http = {.request = unexpected_http};
  fixture->http = (h2_pal_http_api_t){fixture, &http};
  fixture->config = (h2_alipay_client_config_t){
      .mem = &fixture->memory.api,
      .pref = &fixture->pref.api,
      .legacy_pref = &fixture->pref.api,
      .time = &fixture->time.api,
      .crypto = h2_wolfcrypt_crypto_api(),
      .http = &fixture->http,
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .product_type = 3u,
      .feature = 0x4001u,
      .compile_timestamp_s = 1794449000u,
      .pref_namespace = "current",
      .legacy_pref_namespace = "legacy"};
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  uint8_t key[32], first[144], seed[144];
  for (size_t index = 0u; index < sizeof(key); ++index)
    key[index] = (uint8_t)index;
  const uint8_t iv[16] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0x45, 0x72,
                          0x49, 0x55, 0x6a, 0x59, 0x37, 0x57, 0x6b, 0x6d};
  const uint8_t plain[] =
      "1234567890###00112233445566778899AABBCCDDEEFF00112233###60###";
  size_t first_length = 0u, seed_length = 0u;
  assert(h2_crypto_aes_cbc_pkcs7_encrypt(
             fixture->config.crypto, &fixture->memory.api, key, 16u, iv, plain,
             sizeof(plain) - 1u, first, sizeof(first), &first_length) == 0);
  assert(h2_crypto_aes_cbc_pkcs7_encrypt(
             fixture->config.crypto, &fixture->memory.api, key + 16u, 16u, iv,
             first, first_length, seed, sizeof(seed), &seed_length) == 0);
  assert(ALIPAY_deprecated_storage_write(2u, key, sizeof(key)) == 0);
  assert(ALIPAY_deprecated_storage_write(4u, seed, (uint32_t)seed_length) == 0);
  assert(ALIPAY_deprecated_storage_write(16u, (const uint8_t*)"LEGACY_NAME",
                                         12u) == 0);
  assert(ALIPAY_deprecated_storage_write(32u, (const uint8_t*)"LEGACY_LOGIN",
                                         13u) == 0);
  assert(ALIPAY_deprecated_storage_write(66u, (const uint8_t*)"TEST_UID", 8u) ==
         0);
  const uint8_t flag[4] = {0xa5u, 0u, 0u, 0u};
  assert(ALIPAY_deprecated_storage_write(64u, flag, sizeof(flag)) == 0);
  assert(ALIPAY_deprecated_storage_write(65u, (const uint8_t*)"KEEP", 4u) == 0);
}

static void finish(fixture_t* fixture) {
  fixture->memory.fail_at = 0u;
  assert(fixture->pref.active_handles == 0u);
  uint8_t kept[4];
  uint32_t size = sizeof(kept);
  assert(ALIPAY_deprecated_storage_read(65u, kept, &size) == 0);
  assert(size == 4u && memcmp(kept, "KEEP", 4u) == 0);
  assert(h2_alipay_independent_release() == 0);
  assert(h2_app_test_pref_deinit(&fixture->pref) == 0);
  assert(fixture->memory.live_blocks == 0u);
}

int main(void) {
  uint32_t random = 1u;
  const h2_wolfcrypt_crypto_config_t crypto = {&random, entropy};
  assert(h2_wolfcrypt_crypto_init(&crypto) == 0);
  fixture_t fixture;
  initialize(&fixture);
  size_t start = fixture.memory.calls;
  assert(alipay_migrate_data() == 0);
  size_t allocations = fixture.memory.calls - start;
  assert(alipay_get_binding_status());
  h2_alipay_client_binding_t binding;
  assert(h2_alipay_independent_binding_snapshot(&binding) == 0);
  assert(binding.state == H2_ALIPAY_CLIENT_COMPLETE &&
         binding.tid_length == 0u && binding.uid_length == 8u);
  uint8_t name[16], payer[32];
  memset(name, 0xa5, sizeof(name));
  uint32_t size = sizeof(name);
  assert(alipay_get_nick_name(name, &size) == 0);
  assert(size == 12u && memcmp(name, "LEGACY_NAME", 12u) == 0 &&
         name[12] == 0xa5u);
  size = sizeof(payer);
  assert(alipay_get_paycode(payer, &size) == 0 && size == 18u);
  assert(ALIPAY_deprecated_storage_exists(64u) == H2_PAL_ERR_NOT_FOUND);
  assert(alipay_migrate_data() == 99);
  finish(&fixture);
  for (size_t fail = 1u; fail <= allocations; ++fail) {
    initialize(&fixture);
    fixture.memory.fail_at = fixture.memory.calls + fail;
    int rc = alipay_migrate_data();
    fixture.memory.fail_at = 0u;
    assert(rc == 0 || rc == 99);
    if (rc == 0) assert(alipay_get_binding_status());
    finish(&fixture);
  }
  initialize(&fixture);
  fixture.pref.commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  assert(alipay_migrate_data() == 99 && !alipay_get_binding_status());
  assert(ALIPAY_deprecated_storage_exists(64u) == 0);
  finish(&fixture);
  initialize(&fixture);
  const uint8_t invalid[16] = {0};
  assert(ALIPAY_deprecated_storage_write(4u, invalid, sizeof(invalid)) == 0);
  assert(alipay_migrate_data() == 99 && !alipay_get_binding_status());
  assert(ALIPAY_deprecated_storage_exists(64u) == 0);
  finish(&fixture);
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
