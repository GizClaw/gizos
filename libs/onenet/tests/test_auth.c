#include "h2_onenet.h"
#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <string.h>

static int unused_entropy(void* user, uint8_t* out, size_t len) {
  (void)user;
  (void)out;
  /* Signing is deterministic and must not need RNG state. */
  return len == 0u ? H2_PAL_OK : H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t failed_sign(void* user, const uint8_t* key,
                                   size_t key_len, const uint8_t* input,
                                   size_t input_len, uint8_t* digest) {
  (void)user;
  (void)key;
  (void)key_len;
  (void)input;
  (void)input_len;
  (void)digest;
  return -1000;
}

int main(void) {
  const h2_wolfcrypt_crypto_config_t config = {.entropy = unused_entropy};
  assert(h2_wolfcrypt_crypto_init(&config) == H2_PAL_OK);
  char token[H2_ONENET_TOKEN_CAPACITY];
  /* Independent Python hashlib/HMAC reference for the documented signing
   * sequence, including URI escaping of the resource and Base64 signature. */
  const char expected[] =
      "version=2018-10-31&res=products%2Fproduct-1%2Fdevices%2Fdevice%3A01"
      "&et=2000000000&method=sha1&sign=k3qAEGt%2BPd6TC0EtyL6A1LzRxVg%3D";
  assert(h2_onenet_token_generate(h2_wolfcrypt_crypto_api(), "product-1",
                                  "device:01", "ZXhhbXBsZS1kZXZpY2Uta2V5",
                                  UINT64_C(2000000000), token,
                                  sizeof(token)) == H2_PAL_OK);
  assert(strcmp(token, expected) == 0);
  for (size_t capacity = 1u; capacity <= strlen(expected); ++capacity) {
    memset(token, 'x', sizeof(token));
    assert(h2_onenet_token_generate(h2_wolfcrypt_crypto_api(), "product-1",
                                    "device:01", "ZXhhbXBsZS1kZXZpY2Uta2V5",
                                    UINT64_C(2000000000), token,
                                    capacity) == H2_PAL_ERR_NO_SPACE);
    assert(token[0] == '\0');
  }
  const char* invalid_keys[] = {"", "?", "Zg=", "Zh==", " Zg==", "Zg==tail"};
  for (size_t i = 0u; i < sizeof(invalid_keys) / sizeof(invalid_keys[0]); ++i) {
    assert(h2_onenet_token_generate(h2_wolfcrypt_crypto_api(), "p", "d",
                                    invalid_keys[i], 1u, token,
                                    sizeof(token)) != H2_PAL_OK);
    assert(token[0] == '\0');
  }
  assert(h2_onenet_token_generate(NULL, "p", "d", "Zg==", 1u, token,
                                  sizeof(token)) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_onenet_token_generate(NULL, "p/other", "d", "Zg==", 1u, token,
                                  sizeof(token)) == H2_PAL_ERR_INVALID_ARG);
  const h2_pal_crypto_vtable_t vtable = {.hmac_sha1 = failed_sign};
  const h2_pal_crypto_api_t failed = {.vtable = &vtable};
  assert(h2_onenet_token_generate(&failed, "p", "d", "Zg==", 1u, token,
                                  sizeof(token)) == -1000);
  assert(token[0] == '\0');
  h2_wolfcrypt_crypto_deinit();

  char client_id[H2_ONENET_CLIENT_ID_CAPACITY];
  h2_onenet_identity_t identity = {
      .network = 1u, .imei = "490154203237518", .imsi = "460001234567890"};
  assert(h2_onenet_build_client_id("device-1", &identity, client_id,
                                   sizeof(client_id)) == H2_PAL_OK);
  assert(strcmp(client_id,
                "device-1?network=1&imei=490154203237518"
                "&imsi=460001234567890") == 0);
  identity.imsi = NULL;
  identity.iccid = "89860012345678901234";
  identity.msisdn = "13800138000";
  assert(h2_onenet_build_client_id("device-1", &identity, client_id,
                                   sizeof(client_id)) == H2_PAL_OK);
  identity.iccid = NULL;
  identity.msisdn = NULL;
  assert(h2_onenet_build_client_id("device-1", &identity, client_id,
                                   sizeof(client_id)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(client_id[0] == '\0');
  identity = (h2_onenet_identity_t){.network = 0u, .mac = "AA:BB:CC:DD:EE:FF"};
  assert(h2_onenet_build_client_id("device-1", &identity, client_id,
                                   sizeof(client_id)) == H2_PAL_OK);
  assert(strcmp(client_id,
                "device-1?network=0&mac=AA%3ABB%3ACC%3ADD%3AEE%3AFF") == 0);
  identity.imei = "490154203237518";
  assert(h2_onenet_build_client_id("device-1", &identity, client_id,
                                   sizeof(client_id)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_onenet_build_client_id("device/#", NULL, client_id,
                                   sizeof(client_id)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_onenet_build_client_id("device-1", NULL, client_id, 3u) ==
         H2_PAL_ERR_NO_SPACE);
  assert(client_id[0] == '\0');
  return 0;
}
