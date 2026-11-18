#include "h2_alipay_context.h"
#include "h2_alipay_api.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <string.h>

typedef struct fixture {
  unsigned registrations, preparations, queries, finishes, unbinds,
      remote_queries;
  bool completed;
  int network_rc;
  const char* remote_response;
  unsigned progress_calls;
  int progress_status;
} fixture_t;

static void binding_progress(void* user, int status) {
  fixture_t* fixture = user;
  ++fixture->progress_calls;
  fixture->progress_status = status;
  h2_alipay_independent_binding_result_t result;
  assert(h2_alipay_independent_last_binding_result(&result) == 0);
}

static void legacy_store(h2_app_test_pref_t* pref, h2_app_test_mem_t* mem) {
  static const uint32_t items[] = {0u, 4u, 66u, UINT32_MAX};
  uint8_t value[80], output[82];
  for (size_t item = 0u; item < 4u; ++item) {
    memset(value, (int)(item + 1u), sizeof(value));
    assert(ALIPAY_deprecated_storage_exists(items[item]) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(ALIPAY_deprecated_storage_write(items[item], value, sizeof(value)) ==
           0);
    assert(ALIPAY_deprecated_storage_exists(items[item]) == 0);
    for (uint32_t capacity = 0u; capacity <= sizeof(value); ++capacity) {
      memset(output, 0xa5, sizeof(output));
      uint32_t length = capacity;
      int rc =
          ALIPAY_deprecated_storage_read(items[item], output + 1u, &length);
      assert(length == sizeof(value));
      assert(rc == (capacity < sizeof(value) ? H2_PAL_ERR_NO_SPACE : 0));
      assert(output[0] == 0xa5u && output[81] == 0xa5u);
      if (rc == 0)
        assert(memcmp(output + 1u, value, sizeof(value)) == 0);
      else
        for (size_t index = 0u; index < sizeof(output); ++index)
          assert(output[index] == 0xa5u);
    }
  }
  uint32_t length = sizeof(output);
  pref->read = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  memset(output, 0xa5, sizeof(output));
  assert(ALIPAY_deprecated_storage_read(4u, output, &length) == H2_PAL_ERR_IO);
  assert(output[0] == 0xa5u && length == sizeof(output));
  pref->commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  assert(ALIPAY_deprecated_storage_delete(4u) == H2_PAL_ERR_IO);
  assert(ALIPAY_deprecated_storage_exists(4u) == 0);
  pref->commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  memset(value, 0xff, sizeof(value));
  assert(ALIPAY_deprecated_storage_write(4u, value, sizeof(value)) ==
         H2_PAL_ERR_IO);
  length = sizeof(output);
  assert(ALIPAY_deprecated_storage_read(4u, output, &length) == 0);
  assert(length == sizeof(value) && output[0] == 2u);
  size_t baseline = mem->live_blocks;
  for (size_t fail = 1u; fail < 32u; ++fail) {
    size_t before = mem->calls;
    mem->fail_at = before + fail;
    length = sizeof(output);
    int rc = ALIPAY_deprecated_storage_read(4u, output, &length);
    bool reached = mem->calls >= before + fail;
    mem->fail_at = 0u;
    assert(mem->live_blocks == baseline && pref->active_handles == 0u);
    if (!reached) {
      assert(rc == 0);
      break;
    }
    assert(rc == H2_PAL_ERR_NO_MEMORY);
    assert(fail != 31u);
  }
  for (size_t item = 0u; item < 4u; ++item) {
    assert(ALIPAY_deprecated_storage_delete(items[item]) == 0);
    assert(ALIPAY_deprecated_storage_exists(items[item]) ==
           H2_PAL_ERR_NOT_FOUND);
  }
  assert(ALIPAY_deprecated_storage_write(1u, NULL, 0u) == 0);
  length = 0u;
  assert(ALIPAY_deprecated_storage_read(1u, NULL, &length) == 0 &&
         length == 0u);
  assert(ALIPAY_deprecated_storage_exists(1u) == 0);
  assert(ALIPAY_deprecated_storage_delete(1u) == 0);
  assert(ALIPAY_deprecated_storage_read(4u, NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(ALIPAY_deprecated_storage_write(4u, NULL, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  const uint8_t persistent[] = {0u, 0x66u, 0xffu};
  assert(ALIPAY_deprecated_storage_write(66u, persistent, sizeof(persistent)) ==
         0);
}

static void sync_faults(h2_app_test_mem_t* memory, h2_app_test_pref_t* pref) {
  static const uint8_t input[] = "{\"cmd\":4,\"requestId\":\"FAULT\"}";
  static const char expected[] =
      "{\n\t\"version\":\t\"1.0.0.20229511\",\n\t\"result\":\t\"0\","
      "\n\t\"requestId\":\t\"FAULT\"\n}";
  uint8_t* output = NULL;
  uint32_t length = 0u;
  size_t before = memory->calls;
  assert(alipay_biz_process_entry(input, sizeof(input) - 1u, &output,
                                  &length) == 0);
  size_t allocations = memory->calls - before;
  assert(length == sizeof(expected) - 1u &&
         memcmp(output, expected, sizeof(expected)) == 0);
  size_t baseline = memory->live_blocks;
  for (size_t fail = 1u; fail <= allocations; ++fail) {
    uint8_t sentinel = 0xa5u;
    output = &sentinel;
    length = 777u;
    memory->fail_at = memory->calls + fail;
    int rc =
        alipay_biz_process_entry(input, sizeof(input) - 1u, &output, &length);
    memory->fail_at = 0u;
    assert(rc == 29 || rc == 99);
    assert(output == &sentinel && length == 777u && sentinel == 0xa5u);
    assert(memory->live_blocks == baseline && pref->active_handles == 0u);
  }
  assert(alipay_biz_process_entry(NULL, 2048u, NULL, NULL) ==
         (retval_e)BIZ_CMD_UNKNOWN);
  assert(alipay_biz_process_entry(NULL, 1u, NULL, NULL) ==
         ALIPAY_RV_WRONG_PARAM);
}

static void static_cipher_init(h2_app_test_mem_t* memory) {
  size_t baseline = memory->live_blocks;
  memory->fail_at = memory->calls + 1u;
  assert(alipay_npay_ble_broadcast_init() == 30);
  memory->fail_at = 0u;
  assert(memory->live_blocks == baseline);
  assert(alipay_npay_ble_broadcast_init() == 0);
  assert(memory->live_blocks == baseline + 1u);
  size_t calls = memory->calls;
  memory->fail_at = calls + 1u;
  assert(alipay_npay_ble_broadcast_init() == 0 && memory->calls == calls);
  memory->fail_at = 0u;
}

static void static_cipher_output(h2_app_test_mem_t* memory) {
  assert(h2_alipay_independent_set_nfc_context("TOKEN123", "SQUEEZE123",
                                               "123456789012345678") == 0);
  uint8_t output[128];
  memset(output, 0xa5, sizeof(output));
  uint32_t size = 0u;
  assert(alipay_get_paycode_with_nfc_token((uint8_t*)"TOKEN123", 1u, output,
                                           &size) == 0);
  assert(size == 44u && output[44] == 0xa5u);
  size_t baseline = memory->live_blocks;
  memset(output, 0xa5, sizeof(output));
  size = 7u;
  memory->fail_at = memory->calls + 1u;
  assert(alipay_get_paycode_with_nfc_token((uint8_t*)"TOKEN123", 8u, output,
                                           &size) == 99);
  memory->fail_at = 0u;
  assert(size == 7u && memory->live_blocks == baseline);
  for (size_t index = 0u; index < sizeof(output); ++index)
    assert(output[index] == 0xa5u);
  assert(alipay_get_binding_status());
}

static const char factory_receipt[] =
    "{\"success\":true,\"deviceCertificate\":{},"
    "\"privateKey\":\"SYNTHETIC_PRIVATE\"}";

static void certificate_snapshot(h2_app_test_mem_t* memory,
                                 h2_app_test_pref_t* pref) {
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&pref->api, "independent_test",
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  void* value = NULL;
  size_t length = 0u;
  assert(namespace->get_blob(namespace, &memory->api, "device_certificate",
                             &value, &length) == 0);
  assert(length == sizeof(factory_receipt) - 1u &&
         memcmp(value, factory_receipt, length) == 0);
  h2_pal_mem_free(&memory->api, value);
  assert(namespace->close(namespace) == 0);
}

static void provision_faults(h2_app_test_mem_t* memory,
                             h2_app_test_pref_t* pref) {
  assert(alipay_direct_provision(factory_receipt) == 0);
  size_t first = memory->calls;
  assert(alipay_direct_provision(factory_receipt) == 0);
  size_t allocations = memory->calls - first;
  size_t baseline = memory->live_blocks;
  for (size_t offset = 1u; offset <= allocations; ++offset) {
    size_t before = memory->calls;
    memory->fail_at = before + offset;
    int rc = alipay_direct_provision(factory_receipt);
    bool reached = memory->calls >= before + offset;
    memory->fail_at = 0u;
    assert(reached ? (rc == 29 || rc == 5 || rc == 99) : rc == 0);
    assert(memory->live_blocks == baseline && pref->active_handles == 0u);
    certificate_snapshot(memory, pref);
  }
  pref->commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  assert(alipay_direct_provision(
             "{\"success\":true,\"deviceCertificate\":{},"
             "\"privateKey\":\"DIFFERENT_SYNTHETIC_PRIVATE\"}") == 5);
  assert(memory->live_blocks == baseline && pref->active_handles == 0u);
  certificate_snapshot(memory, pref);
}

static void receive_limits(h2_app_test_mem_t* memory,
                           h2_app_test_pref_t* pref) {
  assert(alipay_nfc_recv_data_handle(NULL, 0u) == 99);
  assert(alipay_npay_ble_recv_data_handle(NULL, 0u) == 99);
  uint8_t frame[32] = {0};
  assert(alipay_npay_ble_recv_data_handle(frame, 9u) == 99);
  memset(frame + 6u, 0xff, 4u);
  assert(alipay_npay_ble_recv_data_handle(frame, 10u) == 99);
  memset(frame, 0, sizeof(frame));
  frame[9] = 9u;
  memcpy(frame + 10u, "a,b,c,d,e", 9u);
  size_t baseline = memory->live_blocks;
  memory->fail_at = memory->calls + 1u;
  assert(alipay_npay_ble_recv_data_handle(frame, 19u) == 30);
  memory->fail_at = 0u;
  assert(memory->live_blocks == baseline && pref->active_handles == 0u);
  assert(alipay_npay_ble_recv_data_handle(frame, 19u) == 52);
  assert(memory->live_blocks == baseline && pref->active_handles == 0u);
}

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  fixture_t* fixture = user;
  bool registration = false, query = false, finish = false, unbind = false;
  bool remote = false;
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
    if (header->name.len == sizeof("Operation-Type") - 1u &&
        memcmp(header->name.data, "Operation-Type", header->name.len) == 0)
      unbind = header->value.len ==
               sizeof("alipay.mobile.alideviceinfo.changecode.unbind") - 1u;
    if (header->name.len == sizeof("Operation-Type") - 1u &&
        memcmp(header->name.data, "Operation-Type", header->name.len) == 0)
      remote =
          header->value.len ==
          sizeof(
              "alipay.mobile.alideviceinfo.changecode.device.queryDeviceInfo") -
              1u;
  }
  if (registration)
    ++fixture->registrations;
  else if (query)
    ++fixture->queries;
  else if (finish)
    ++fixture->finishes;
  else if (unbind)
    ++fixture->unbinds;
  else if (remote)
    ++fixture->remote_queries;
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
      : remote     ? (const uint8_t*)fixture->remote_response
      : query      ? (fixture->completed ? (const uint8_t*)completed : pending)
      : finish || unbind ? (const uint8_t*)"{\"success\":true}"
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
      .gateway_url = "https://iotapi.alipay.com/mgw.htm",
      .identity = {.sn = "TEST_SN_001",
                   .imei = "TEST_SN_001",
                   .mac = "12:34:56:78:9A:BC",
                   .model = "H106"},
      .company = "HAIVIVI",
      .product_type = 3u,
      .pref_namespace = "independent_test",
      .compile_timestamp_s = 1794449000u,
      .legacy_pref = &pref.api,
      .legacy_pref_namespace = "independent_legacy",
      .binding_progress = binding_progress,
      .binding_progress_user = &fixture,
  };
  h2_alipay_client_config_t invalid = config;
  invalid.legacy_pref_namespace = NULL;
  assert(h2_alipay_independent_configure(&invalid, &sync.api) ==
         H2_PAL_ERR_INVALID_ARG);
  sync.create =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_NO_MEMORY, .remaining = 1u};
  assert(h2_alipay_independent_configure(&config, &sync.api) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(h2_alipay_independent_configure(&config, &sync.api) == H2_PAL_OK);
  assert(h2_alipay_independent_configure(&config, &sync.api) ==
         H2_PAL_ERR_INVALID_STATE);
  static_cipher_init(&mem);
  provision_faults(&mem, &pref);
  legacy_store(&pref, &mem);
  alipay_binding_process_notify(10);
  alipay_binding_process_notify(70);
  alipay_binding_process_notify(-1);
  assert(fixture.progress_calls == 3u && fixture.progress_status == -1);
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
  sync_faults(&mem, &pref);
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
  static_cipher_output(&mem);
  receive_limits(&mem, &pref);
  typedef retval_e (*account_getter_t)(uint8_t*, uint32_t*);
  const account_getter_t account_getters[] = {alipay_get_logon_ID,
                                              alipay_get_nick_name};
  const char* account_values[] = {"TEST_LOGIN", "TEST_NAME"};
  for (size_t field = 0u; field < 2u; ++field) {
    uint8_t output[144];
    memset(output, 0x5a, sizeof(output));
    uint32_t size = (uint32_t)strlen(account_values[field]);
    assert(account_getters[field](output, &size) == 0);
    assert(size == strlen(account_values[field]));
    assert(memcmp(output, account_values[field], size) == 0);
    assert(output[size] == 0x5au);
    --size;
    memset(output, 0x5a, sizeof(output));
    assert(account_getters[field](output, &size) == 4);
    assert(size == strlen(account_values[field]) && output[0] == 0x5au);
    assert(account_getters[field](NULL, &size) == ALIPAY_RV_WRONG_PARAM);
    assert(account_getters[field](output, NULL) == ALIPAY_RV_WRONG_PARAM);
    pref.read = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
    size = sizeof(output);
    assert(account_getters[field](output, &size) == 4);
    assert(size == sizeof(output) && output[0] == 0x5au);
  }
  assert(h2_alipay_independent_release() == H2_PAL_OK);
  assert(h2_alipay_independent_configure(&config, &sync.api) == 0);
  assert(alipay_pre_init() == ALIPAY_RV_OK && alipay_get_binding_status());
  unsigned queries = fixture.queries, finishes = fixture.finishes;
  assert(alipay_query_binding_result(&status) == ALIPAY_RV_OK &&
         status == ALIPAY_STATUS_BINDING_OK);
  assert(fixture.queries == queries && fixture.finishes == finishes);
  fixture.remote_response = "{\"watchBindStatus\":\"BINDED\"}";
  alipay_remote_bind_status_t remote_status = REMOTE_STATUS_UNKNOWN;
  size_t live_blocks = mem.live_blocks;
  size_t allocation_start = mem.calls;
  assert(alipay_query_remote_bind_status(&remote_status) == ALIPAY_RV_OK);
  size_t allocation_count = mem.calls - allocation_start;
  assert(remote_status == REMOTE_STATUS_BIND && alipay_get_binding_status());
  for (size_t offset = 1u; offset <= allocation_count; ++offset) {
    mem.fail_at = mem.calls + offset;
    int rc = alipay_query_remote_bind_status(&remote_status);
    mem.fail_at = 0u;
    assert(rc == ALIPAY_RV_OK || rc == 99);
    assert(mem.live_blocks == live_blocks);
    assert(alipay_get_binding_status());
  }
  const char* malformed_remote[] = {
      "[]", "{",
      "{\"watchBindStatus\":\"UNBIND\",\"watchBindStatus\":\"BINDED\"}"};
  for (size_t index = 0u; index < 3u; ++index) {
    fixture.remote_response = malformed_remote[index];
    assert(alipay_query_remote_bind_status(&remote_status) == 99);
    assert(alipay_get_binding_status() && fixture.unbinds == 0u);
  }
  fixture.remote_response = "{\"watchBindStatus\":\"BINDED\"}";
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&pref.api, config.pref_namespace,
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  const uint8_t foreign[] = {1u, 2u, 3u};
  assert(namespace->set_blob(namespace, "foreign", foreign, sizeof(foreign)) ==
         0);
  assert(namespace->commit(namespace) == 0 && namespace->close(namespace) == 0);
  pref.commit = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  assert(alipay_unbinding() != ALIPAY_RV_OK);
  assert(alipay_get_binding_status());
  fixture.network_rc = H2_PAL_ERR_IO;
  assert(alipay_unbinding() == ALIPAY_RV_OK);
  assert(!alipay_get_binding_status() && fixture.unbinds == 2u);
  fixture.network_rc = H2_PAL_OK;
  assert(alipay_unbinding() == ALIPAY_RV_OK && fixture.unbinds == 2u);
  assert(h2_pal_pref_open(&pref.api, config.pref_namespace,
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  void* stored = NULL;
  size_t stored_size = 0u;
  assert(namespace->get_blob(namespace, &mem.api, "foreign", &stored,
                             &stored_size) == 0);
  assert(stored_size == sizeof(foreign) &&
         memcmp(stored, foreign, sizeof(foreign)) == 0);
  h2_pal_mem_free(&mem.api, stored);
  assert(namespace->close(namespace) == 0);
  length = sizeof(code);
  unsigned registrations = fixture.registrations;
  assert(alipay_get_binding_code(FEATURE_PAYCODE | FEATURE_SQUARE, code,
                                 &length) == ALIPAY_RV_OK);
  assert(fixture.registrations == registrations + 1u);
  assert(alipay_reset_all() == ALIPAY_RV_OK);
  assert(alipay_query_binding_result(&status) == ALIPAY_RV_OK);
  assert(status == ALIPAY_STATUS_BINDING_FAIL);
  product_length = sizeof(product_text);
  assert(alipay_get_logon_ID(product_text, &product_length) == 4);
  assert(product_length == sizeof(product_text));
  uint8_t legacy[4] = {0xa5u, 0xa5u, 0xa5u, 0xa5u};
  uint32_t legacy_length = sizeof(legacy);
  assert(ALIPAY_deprecated_storage_read(66u, legacy, &legacy_length) == 0);
  assert(legacy_length == 3u && legacy[0] == 0u && legacy[1] == 0x66u &&
         legacy[2] == 0xffu && legacy[3] == 0xa5u);
  certificate_snapshot(&mem, &pref);
  assert(h2_alipay_independent_release() == H2_PAL_OK);
  h2_alipay_client_config_t disabled = config;
  disabled.legacy_pref = NULL;
  disabled.legacy_pref_namespace = NULL;
  disabled.binding_progress = NULL;
  assert(h2_alipay_independent_configure(&disabled, &sync.api) == 0);
  assert(ALIPAY_deprecated_storage_exists(66u) == H2_PAL_ERR_UNSUPPORTED);
  alipay_binding_process_notify(10);
  assert(fixture.progress_calls == 3u);
  assert(h2_alipay_independent_release() == 0);
  product_length = sizeof(product_text);
  assert(alipay_get_companyName(product_text, &product_length) != ALIPAY_RV_OK);
  assert(alipay_get_productModel(product_text, &product_length) !=
         ALIPAY_RV_OK);
  assert(h2_app_test_pref_deinit(&pref) == H2_PAL_OK);
  assert(mem.live_blocks == 0u);
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
