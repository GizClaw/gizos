#include "h2_alipay_independent.h"
#include "alipay_account_manage.h"
#include "alipay_bind.h"
#include "vendor_device_info.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <string.h>

typedef struct fixture {
  unsigned registrations, preparations, queries, finishes;
  bool completed;
  int network_rc;
} fixture_t;

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  fixture_t* fixture = user;
  bool registration = false, query = false, finish = false;
  for (size_t index = 0u; index < request->header_count; ++index) {
    const h2_pal_http_header_t* header = &request->headers[index];
    if (header->name.len == sizeof("Operation-Type") - 1u &&
        memcmp(header->name.data, "Operation-Type", header->name.len) == 0)
      query =
          header->value.len ==
          sizeof("alipay.mobile.alideviceinfo.changecode.queryBindInfo") - 1u;
    if (header->name.len == sizeof("Operation-Type") - 1u &&
        memcmp(header->name.data, "Operation-Type", header->name.len) == 0)
      finish = header->value.len ==
               sizeof("alipay.mobile.alideviceinfo.changecode.finishBind") - 1u;
    if (header->name.len == sizeof("Operation-Type") - 1u &&
        memcmp(header->name.data, "Operation-Type", header->name.len) == 0)
      registration =
          header->value.len == sizeof("iothub.internal.device.active") - 1u &&
          memcmp(header->value.data, "iothub.internal.device.active",
                 header->value.len) == 0;
  }
  if (registration)
    ++fixture->registrations;
  else if (query)
    ++fixture->queries;
  else if (finish)
    ++fixture->finishes;
  else
    ++fixture->preparations;
  if (fixture->network_rc != H2_PAL_OK) return fixture->network_rc;
  static const uint8_t device_name[] = {0x0a, 7u,  'D', 'E', 'V',
                                        'I',  'C', 'E', '1'};
  static const uint8_t token[] = "{\"success\":true,\"token\":\"TEST_TOKEN\"}";
  static const uint8_t pending[] = "{\"needQuery\":true}";
  static const char completed[] =
      "{\"needQuery\":false,\"success\":true,\"tid\":\"TEST_TID\","
      "\"userId\":\"TEST_UID\",\"nickName\":\"TEST_NAME\",\"loginId\":\"TEST_LOGIN\","
      "\"serverTime\":\"1794449000000\",\"encryptedFactors\":\""
      "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
      "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5&"
      "000102030405060708090a0b0c0d0e0f\"}";
  const uint8_t* bytes =
      registration ? device_name
      : query      ? (fixture->completed ? (const uint8_t*)completed : pending)
      : finish     ? (const uint8_t*)"{\"success\":true}"
                   : token;
  size_t length =
      registration ? sizeof(device_name) : strlen((const char*)bytes);
  assert(length <= request->response_buf_cap);
  memcpy(request->response_buf, bytes, length);
  response->status_code = 200;
  response->body = request->response_buf;
  response->body_len = length;
  return h2_pal_http_deliver_response_header(request, "Result-Status", 13u,
                                             "1000", 4u);
}

static void response_free(void* user, h2_pal_http_response_t* response) {
  (void)user;
  h2_pal_http_response_reset(response);
}

static int random_bytes(void* user, uint8_t* out, size_t length) {
  uint32_t* state = user;
  for (size_t i = 0u; i < length; ++i) {
    *state = *state * 1664525u + 1013904223u;
    out[i] = (uint8_t)(*state >> 24u);
  }
  return H2_PAL_OK;
}

int main(void) {
  assert(alipay_pre_init() != ALIPAY_RV_OK);
  assert(!alipay_get_binding_status());
  uint8_t product_text[24];
  uint32_t product_length = sizeof(product_text);
  assert(alipay_get_companyName(product_text, &product_length) != ALIPAY_RV_OK);
  assert(alipay_get_productModel(product_text, &product_length) !=
         ALIPAY_RV_OK);
  h2_app_test_mem_t mem;
  h2_app_test_mem_init(&mem, NULL);
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &mem.api) == H2_PAL_OK);
  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
  assert(h2_pal_time_set_wall_ms(&time.api, 1794449000000u) == H2_PAL_OK);
  fixture_t fixture = {0};
  const h2_pal_http_vtable_t http_vtable = {.request = request,
                                            .response_free = response_free};
  const h2_pal_http_api_t http = {&fixture, &http_vtable};
  uint32_t entropy = 1u;
  const h2_wolfcrypt_crypto_config_t crypto_config = {&entropy, random_bytes};
  assert(h2_wolfcrypt_crypto_init(&crypto_config) == H2_PAL_OK);
  const h2_alipay_client_config_t config = {
      .mem = &mem.api,
      .pref = &pref.api,
      .time = &time.api,
      .crypto = h2_wolfcrypt_crypto_api(),
      .http = &http,
      .identity = {.sn = "TEST_SN_001",
                   .imei = "TEST_SN_001",
                   .mac = "12:34:56:78:9A:BC",
                   .model = "H106"},
      .company = "HAIVIVI",
      .product_type = 3u,
      .pref_namespace = "independent_test",
      .compile_timestamp_s = 1794449000u,
  };
  sync.create =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_NO_MEMORY, .remaining = 1u};
  assert(h2_alipay_independent_configure(&config, &sync.api) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(h2_alipay_independent_configure(&config, &sync.api) == H2_PAL_OK);
  assert(h2_alipay_independent_configure(&config, &sync.api) ==
         H2_PAL_ERR_INVALID_STATE);
  product_length = sizeof(product_text);
  assert(alipay_get_companyName(product_text, &product_length) == ALIPAY_RV_OK);
  assert(product_length == 7u && strcmp((char*)product_text, "HAIVIVI") == 0);
  product_length = sizeof(product_text);
  assert(alipay_get_productModel(product_text, &product_length) ==
         ALIPAY_RV_OK);
  assert(product_length == 4u && strcmp((char*)product_text, "H106") == 0);
  memset(product_text, 0x5a, sizeof(product_text));
  product_length = 4u;
  assert(alipay_get_productModel(product_text, &product_length) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(product_text[0] == 0x5au && product_length == 4u);
  product_length = 5u;
  assert(alipay_get_productModel(product_text, &product_length) ==
         ALIPAY_RV_OK);
  assert(product_length == 4u && product_text[4] == '\0');
  assert(strcmp((char*)product_text, "H106") == 0);
  assert(alipay_get_companyName(NULL, &product_length) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(alipay_get_productModel(product_text, NULL) == ALIPAY_RV_WRONG_PARAM);
  assert(alipay_get_productType() == 3u);
  typedef retval_e (*getter_t)(uint8_t*, uint32_t*);
  const getter_t getters[] = {alipay_get_sn, alipay_get_imei, alipay_get_mac};
  const char* expected[] = {config.identity.sn, config.identity.imei,
                            config.identity.mac};
  for (size_t i = 0u; i < 3u; ++i) {
    uint8_t buffer[49];
    uint32_t size = sizeof(buffer);
    assert(getters[i](buffer, &size) == ALIPAY_RV_OK);
    assert(size == strlen(expected[i]) &&
           strcmp((char*)buffer, expected[i]) == 0);
    size = (uint32_t)strlen(expected[i]);
    memset(buffer, 0x5a, sizeof(buffer));
    assert(getters[i](buffer, &size) == ALIPAY_RV_WRONG_PARAM);
    assert(buffer[0] == 0x5au && size == strlen(expected[i]));
    assert(getters[i](NULL, &size) == ALIPAY_RV_WRONG_PARAM);
  }
  sync.lock = (h2_app_test_fault_t){.result = H2_PAL_ERR_BUSY, .remaining = 1u};
  assert(alipay_get_companyName(product_text, &product_length) ==
         ALIPAY_RV_HAL_ERROR);
  assert(fixture.registrations == 0u && fixture.preparations == 0u);
  char code[256] = "old-code";
  int length = sizeof(code);
  assert(alipay_get_binding_code(FEATURE_PAYCODE | FEATURE_SQUARE, code,
                                 &length) != ALIPAY_RV_OK);
  assert(length == 0 && code[0] == '\0' && fixture.registrations == 0u);
  assert(alipay_pre_init() == ALIPAY_RV_OK);
  assert(alipay_pre_init() == ALIPAY_RV_OK);
  length = sizeof(code);
  assert(alipay_get_binding_code(FEATURE_PAYCODE | FEATURE_SQUARE, code,
                                 &length) == ALIPAY_RV_OK);
  assert(length > 0 && (size_t)length == strlen(code));
  assert(strstr(code, "ALIPAY_QZWT#0#TEST_TOKEN#") == code);
  assert(fixture.registrations == 1u && fixture.preparations == 1u);
  binding_status_e status = ALIPAY_STATUS_BINDING_OK;
  assert(alipay_query_binding_result(&status) == ALIPAY_RV_OK);
  assert(status == ALIPAY_STATUS_UNKNOWN && fixture.queries == 1u);
  assert(fixture.preparations == 1u);
  sync.destroy =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_BUSY, .remaining = 1u};
  assert(h2_alipay_independent_release() == H2_PAL_ERR_BUSY);
  assert(alipay_pre_init() == ALIPAY_RV_OK);
  assert(h2_alipay_independent_release() == H2_PAL_OK);
  assert(h2_alipay_independent_release() == H2_PAL_OK);
  assert(h2_alipay_independent_configure(&config, &sync.api) == H2_PAL_OK);
  assert(alipay_pre_init() == ALIPAY_RV_OK);
  length = sizeof(code);
  assert(alipay_get_binding_code(FEATURE_PAYCODE | FEATURE_SQUARE, code,
                                 &length) == ALIPAY_RV_OK);
  assert(fixture.registrations == 1u && fixture.preparations == 2u);
  fixture.network_rc = H2_PAL_ERR_IO;
  length = sizeof(code);
  assert(alipay_get_binding_code(FEATURE_PAYCODE | FEATURE_SQUARE, code,
                                 &length) != ALIPAY_RV_OK);
  assert(code[0] == '\0' && length == 0);
  fixture.network_rc = H2_PAL_OK;
  length = sizeof(code);
  assert(alipay_get_binding_code(FEATURE_PAYCODE | FEATURE_SQUARE, code,
                                 &length) == ALIPAY_RV_OK);
  fixture.completed = true;
  assert(alipay_query_binding_result(&status) == ALIPAY_RV_OK);
  assert(status == ALIPAY_STATUS_BINDING_OK && fixture.finishes == 1u);
  assert(alipay_get_binding_status());
  h2_alipay_independent_binding_result_t diagnostics;
  assert(h2_alipay_independent_last_binding_result(&diagnostics) == 0);
  assert(diagnostics.binding.state == H2_ALIPAY_CLIENT_COMPLETE &&
         diagnostics.result == 0);
  assert(h2_alipay_independent_release() == H2_PAL_OK);
  assert(h2_alipay_independent_configure(&config, &sync.api) == 0);
  assert(alipay_pre_init() == ALIPAY_RV_OK && alipay_get_binding_status());
  unsigned queries = fixture.queries, finishes = fixture.finishes;
  assert(alipay_query_binding_result(&status) == ALIPAY_RV_OK &&
         status == ALIPAY_STATUS_BINDING_OK);
  assert(fixture.queries == queries && fixture.finishes == finishes);
  assert(h2_alipay_independent_release() == H2_PAL_OK);
  product_length = sizeof(product_text);
  assert(alipay_get_companyName(product_text, &product_length) != ALIPAY_RV_OK);
  assert(alipay_get_productModel(product_text, &product_length) !=
         ALIPAY_RV_OK);
  assert(h2_app_test_pref_deinit(&pref) == H2_PAL_OK);
  assert(mem.live_blocks == 0u);
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
