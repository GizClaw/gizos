#include "h2_alipay_context.h"

#include "h2_alipay_client_internal.h"
#include "h2_alipay_transit_internal.h"
#include "h2_alipay_api.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct transit_fixture {
  h2_app_test_mem_t memory;
  h2_app_test_pref_t pref;
  h2_app_test_sync_t sync;
  h2_app_test_time_t time;
  h2_alipay_client_config_t config;
  h2_pal_http_api_t http;
  const char* response;
  unsigned requests;
  bool cancel;
} transit_fixture_t;

static const char cards[] =
    "{\"success\":true,\"cardList\":[{\"cardNo\":\"NUMBER\","
    "\"cardType\":\"TYPE\",\"title\":\"TITLE\"}]}";
static const char online[] =
    "{\"success\":true,\"renderData\":{\"expiredTime\":2000000000,"
    "\"certServiceType\":\"SYNTHETIC\",\"authMode\":\"ONLINE\","
    "\"renderType\":\"ONLINE\",\"qrCode\":\"41424300FF\","
    "\"config\":\"{\\\"QR_MG_TIM\\\":\\\"100\\\",\\\"QR_OG_TIM\\\":\\\"20\\\"}\"}}";
static const char native[] =
    "{\"success\":true,\"renderData\":{\"expiredTime\":2000000000,"
    "\"certServiceType\":\"ALIPAY_OFFLINE_CERT_V2\","
    "\"authMode\":\"OFFLINE\",\"renderType\":\"NATIVE\","
    "\"offlineCert\":\"1122334455667788\","
    "\"certPrivateKey\":\"000000000000000000000000000000000000000000000001\","
    "\"config\":\"{\\\"QR_MG_TIM\\\":\\\"100\\\",\\\"QR_OG_TIM\\\":\\\"20\\\"}\"}}";
static const char native_sm2[] =
    "{\"success\":true,\"renderData\":{\"expiredTime\":2000000000,"
    "\"certServiceType\":\"MOT_OFFLINE_CERT_DOUBLE_SM2\","
    "\"authMode\":\"OFFLINE\",\"renderType\":\"NATIVE\","
    "\"offlineCert\":\"1122334455667788\","
    "\"certPrivateKey\":"
    "\"0000000000000000000000000000000000000000000000000000000000000001\","
    "\"config\":\"{\\\"QR_MG_TIM\\\":\\\"100\\\",\\\"QR_OG_TIM\\\":\\\"20\\\"}\"}}";
static const char native_yct[] =
    "{\"success\":true,\"renderData\":{\"expiredTime\":2000000000,"
    "\"certServiceType\":\"YCT_OFFLINE_CERT_V1\","
    "\"authMode\":\"OFFLINE\",\"renderType\":\"NATIVE\","
    "\"certPrivateKey\":\"{\\\"account_issue_sub_key\\\":"
    "\\\"000102030405060708090A0B0C0D0E0F\\\","
    "\\\"terminal_authen_sub_key\\\":"
    "\\\"101112131415161718191A1B1C1D1E1F\\\"}\","
    "\"offlineCert\":\"{\\\"pay_type\\\":\\\"01\\\","
    "\\\"cert_start_time\\\":\\\"00000000\\\","
    "\\\"cert_data\\\":\\\"112233445566778899AABBCC\\\"}\","
    "\"config\":\"{\\\"QR_MG_TIM\\\":\\\"100\\\",\\\"QR_OG_TIM\\\":\\\"20\\\"}\"}}";
static const char native_rct[] =
    "{\"success\":true,\"renderData\":{\"expiredTime\":2000000000,"
    "\"certServiceType\":\"RCT_OFFLINE_CERT_V1\","
    "\"authMode\":\"OFFLINE\",\"renderType\":\"NATIVE\","
    "\"certPrivateKey\":"
    "\"0000000000000000000000000000000000000000000000000000000000000001\","
    "\"offlineCert\":\"{\\\"cert_data\\\":\\\"1122334455667788\\\","
    "\\\"platform_sign\\\":\\\"AABBCCDDEEFF\\\","
    "\\\"merchant_user_id\\\":\\\"0011223344556677\\\","
    "\\\"amount_limit\\\":\\\"00000064\\\","
    "\\\"other_biz_info\\\":\\\"0000\\\"}\","
    "\"config\":\"{\\\"QR_MG_TIM\\\":\\\"100\\\",\\\"QR_OG_TIM\\\":\\\"20\\\"}\"}}";
static const char native_script[] =
    "{\"success\":true,\"renderData\":{\"expiredTime\":2000000000,"
    "\"certServiceType\":\"SYNTHETIC_SCRIPT\","
    "\"authMode\":\"OFFLINE\",\"renderType\":\"SCRIPT\","
    "\"certPrivateKey\":\"SYNTHETIC_PRIVATE\","
    "\"offlineCert\":\"41424300FF\","
    "\"genCodeScript\":{\"scriptType\":\"JS\",\"scriptName\":\"SCRIPT\","
    "\"mac\":\"MAC\"},"
    "\"config\":\"{\\\"QR_MG_TIM\\\":\\\"100\\\",\\\"QR_OG_TIM\\\":\\\"20\\\"}\"}}";

static int entropy(void* user, uint8_t* output, size_t length) {
  uint32_t* state = user;
  for (size_t index = 0u; index < length; ++index) {
    *state = *state * 1664525u + 1013904223u;
    output[index] = (uint8_t)(*state >> 24u);
  }
  return H2_PAL_OK;
}

static int request(void* user, const h2_pal_http_request_t* input,
                   h2_pal_http_response_t* output) {
  transit_fixture_t* fixture = user;
  ++fixture->requests;
  assert(input->body_len < 512u && input->cancel_cb != NULL);
  if (fixture->cancel) {
    alipay_break_rpc();
    assert(h2_pal_http_request_is_canceled(input));
  }
  const char* response = fixture->response;
  static const char script_operation[] =
      "com.alideviceinfo.transport.query.queryScript";
  for (size_t index = 0u; index < input->header_count; ++index)
    if (input->headers[index].name.len == 14u &&
        memcmp(input->headers[index].name.data, "Operation-Type", 14u) == 0 &&
        input->headers[index].value.len == sizeof(script_operation) - 1u &&
        memcmp(input->headers[index].value.data, script_operation,
               sizeof(script_operation) - 1u) == 0)
      assert(false);
  size_t length = strlen(response);
  assert(length <= input->response_buf_cap);
  memcpy(input->response_buf, response, length);
  output->body = input->response_buf;
  output->body_len = length;
  output->status_code = 200;
  return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                             "1000", 4u);
}

static void initialize(transit_fixture_t* fixture) {
  memset(fixture, 0, sizeof(*fixture));
  h2_app_test_mem_init(&fixture->memory, NULL);
  h2_app_test_sync_init(&fixture->sync);
  h2_app_test_time_init(&fixture->time, 0u);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  assert(h2_app_test_pref_init(&fixture->pref, &fixture->memory.api) == 0);
  static const h2_pal_http_vtable_t http = {.request = request};
  fixture->http = (h2_pal_http_api_t){fixture, &http};
  fixture->config = (h2_alipay_client_config_t){
      .mem = &fixture->memory.api,
      .pref = &fixture->pref.api,
      .time = &fixture->time.api,
      .crypto = h2_wolfcrypt_crypto_api(),
      .http = &fixture->http,
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .product_type = 3u,
      .feature = 0x4001u,
      .compile_timestamp_s = 1794449000u,
      .pref_namespace = "transit"};
  assert(alipay_init_transit_module() == 0);
  assert(h2_alipay_independent_configure(&fixture->config,
                                         &fixture->sync.api) == 0);
  assert(alipay_pre_init() == 0);
  uint8_t context[60] = {0};
  memcpy(context, "APC1", 4u);
  h2_alipay_client_fingerprint(&fixture->config, context + 4u);
  memcpy(context + 36u, "0123456789ABCDEF", 16u);
  context[52] = 7u;
  memcpy(context + 53u, "DEVICE1", 7u);
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "transit",
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  assert(namespace->set_blob(namespace, "context", context, sizeof(context)) ==
         0);
  assert(namespace->set_blob(namespace, "device_certificate", "FACTORY", 7u) ==
         0);
  assert(namespace->commit(namespace) == 0 && namespace->close(namespace) == 0);
  fixture->response = cards;
}

static void finish(transit_fixture_t* fixture) {
  fixture->memory.fail_at = 0u;
  assert(fixture->pref.active_handles == 0u);
  assert(h2_alipay_independent_release() == 0);
  assert(h2_app_test_pref_deinit(&fixture->pref) == 0);
  assert(fixture->memory.live_blocks == 0u);
}

static void read_cards(alipay_tansit_CardBaseVO_t output[2], uint32_t* length,
                       uint32_t* count) {
  memset(output, 0xa5, 2u * sizeof(*output));
  *length = 2u * sizeof(*output);
  *count = 77u;
}

static size_t card_faults(transit_fixture_t* fixture) {
  alipay_tansit_CardBaseVO_t output[2];
  uint32_t length, count;
  initialize(fixture);
  read_cards(output, &length, &count);
  size_t start = fixture->memory.calls;
  assert(alipay_transit_get_card_list_online(output, &length, &count) == 0);
  size_t allocations = fixture->memory.calls - start;
  assert(length == 120u && count == 1u &&
         strcmp(output[0].title, "TITLE") == 0);
  finish(fixture);
  for (size_t fail = 1u; fail <= allocations; ++fail) {
    initialize(fixture);
    read_cards(output, &length, &count);
    fixture->memory.fail_at = fixture->memory.calls + fail;
    int rc = alipay_transit_get_card_list_online(output, &length, &count);
    fixture->memory.fail_at = 0u;
    assert(rc == 0 || rc == 5 || rc == 7 || rc == 29);
    read_cards(output, &length, &count);
    assert(alipay_transit_get_card_list_online(output, &length, &count) == 0);
    finish(fixture);
  }
  return allocations;
}

static size_t data_faults(transit_fixture_t* fixture) {
  initialize(fixture);
  fixture->response = online;
  uint8_t output[528], error[272];
  memset(output, 0xa5, sizeof(output));
  memset(error, 0xa5, sizeof(error));
  uint32_t length = 512u;
  size_t start = fixture->memory.calls;
  assert(alipay_transit_get_TransitCode("NUMBER", "TYPE", output, &length,
                                        (void*)error, 256u) == 0);
  size_t allocations = fixture->memory.calls - start;
  assert(length == 5u && memcmp(output, "ABC\0\xff", 5u) == 0 &&
         output[5] == 0xa5u && error[0] == 0xa5u);
  finish(fixture);
  for (size_t fail = 1u; fail <= allocations; ++fail) {
    initialize(fixture);
    fixture->response = online;
    fixture->memory.fail_at = fixture->memory.calls + fail;
    memset(output, 0xa5, sizeof(output));
    length = 512u;
    int rc = alipay_transit_get_TransitCode("NUMBER", "TYPE", output, &length,
                                            (void*)error, 256u);
    fixture->memory.fail_at = 0u;
    assert(rc == 0 || rc == 29 || rc == 54);
    assert(output[512] == 0xa5u && error[256] == 0xa5u);
    length = 512u;
    assert(alipay_transit_get_TransitCode("NUMBER", "TYPE", output, &length,
                                          (void*)error, 256u) == 0);
    finish(fixture);
  }
  return allocations;
}

static size_t native_faults(transit_fixture_t* fixture, const char* response) {
  initialize(fixture);
  fixture->response = response;
  uint8_t output[528], error[272];
  memset(output, 0xa5, sizeof(output));
  memset(error, 0xa5, sizeof(error));
  uint32_t length = 512u;
  size_t start = fixture->memory.calls;
  assert(alipay_transit_get_TransitCode("NUMBER", "TYPE", output, &length,
                                        (void*)error, 256u) == 0);
  size_t allocations = fixture->memory.calls - start;
  static const uint8_t prefix[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x04, 0x6a, 0xf5, 0x1e, 0x68};
  if (response == native)
    assert(memcmp(output, prefix, sizeof(prefix)) == 0 &&
           length == sizeof(prefix) + 1u + output[sizeof(prefix)]);
  assert(length > 0u && length < 512u);
  assert(output[length] == 0xa5u && error[0] == 0xa5u);
  alipay_transit_card_status_t status;
  assert(alipay_transit_check_card_status("NUMBER", "TYPE", &status) == 0 &&
         status.is_exists && status.remain_use_count == 19);
  finish(fixture);
  for (size_t fail = 1u; fail <= allocations; ++fail) {
    initialize(fixture);
    fixture->response = response;
    fixture->memory.fail_at = fixture->memory.calls + fail;
    length = 512u;
    memset(output, 0xa5, sizeof(output));
    int rc = alipay_transit_get_TransitCode("NUMBER", "TYPE", output, &length,
                                            (void*)error, 256u);
    fixture->memory.fail_at = 0u;
    assert(rc == 0 || rc == 29 || rc == 43 || rc == 54);
    assert(output[512] == 0xa5u && error[256] == 0xa5u);
    length = 512u;
    assert(alipay_transit_get_TransitCode("NUMBER", "TYPE", output, &length,
                                          (void*)error, 256u) == 0);
    finish(fixture);
  }
  return allocations;
}

static void unsupported_script(transit_fixture_t* fixture) {
  initialize(fixture);
  fixture->response = native_script;
  uint8_t output[528], error[272];
  memset(output, 0xa5, sizeof(output));
  memset(error, 0xa5, sizeof(error));
  uint32_t length = 512u;
  assert(alipay_transit_get_TransitCode("NUMBER", "TYPE", output, &length,
                                        (void*)error, 256u) == 43);
  assert(length == 512u && fixture->requests == 1u);
  for (size_t index = 0u; index < sizeof(output); ++index)
    assert(output[index] == 0xa5u);
  assert(error[256] == 0xa5u);
  alipay_transit_card_status_t status;
  assert(alipay_transit_check_card_status("NUMBER", "TYPE", &status) == 0 &&
         status.is_exists && status.remain_use_count == 20);
  assert(alipay_transit_update_card_data("NUMBER", "TYPE", (void*)error,
                                         256u) == 43);
  assert(fixture->requests == 2u && error[256] == 0xa5u);
  finish(fixture);
}

int main(void) {
  uint32_t random = 1u;
  const h2_wolfcrypt_crypto_config_t crypto = {&random, entropy};
  assert(h2_wolfcrypt_crypto_init(&crypto) == 0);
  transit_fixture_t fixture;
  size_t list_faults = card_faults(&fixture);
  size_t code_faults = data_faults(&fixture);
  size_t offline_faults = native_faults(&fixture, native);
  size_t sm2_faults = native_faults(&fixture, native_sm2);
  size_t yct_faults = native_faults(&fixture, native_yct);
  size_t rct_faults = native_faults(&fixture, native_rct);
  unsupported_script(&fixture);
  initialize(&fixture);
  alipay_tansit_CardBaseVO_t output[2];
  uint32_t length, count;
  read_cards(output, &length, &count);
  fixture.cancel = true;
  assert(alipay_transit_get_card_list_online(output, &length, &count) == 7);
  assert(((uint8_t*)output)[0] == 0xa5u && count == 77u);
  assert(alipay_transit_get_card_list_offline(output, &length, &count) == 5);
  fixture.cancel = false;
  assert(alipay_transit_get_card_list_online(output, &length, &count) == 0);
  fixture.response =
      "{\"success\":true,\"cardList\":[{\"cardNo\":\"A\",\"cardType\":\"B\","
      "\"title\":\"C\"},{\"cardNo\":\"D\",\"cardType\":\"E\",\"title\":\"F\"}]}";
  fixture.pref.commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  strcpy(fixture.pref.commit_key, "transit_cards");
  read_cards(output, &length, &count);
  assert(alipay_transit_get_card_list_online(output, &length, &count) == 5);
  read_cards(output, &length, &count);
  assert(alipay_transit_get_card_list_offline(output, &length, &count) == 5 &&
         count == 2u && ((uint8_t*)output)[0] == 0xa5u);
  fixture.response = online;
  char error[256];
  memset(error, 0xa5, sizeof(error));
  assert(alipay_transit_update_card_data("NUMBER", "TYPE", error,
                                         sizeof(error)) == 0);
  strcpy(fixture.pref.commit_key, "transit_data");
  fixture.pref.commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  fixture.response =
      "{\"success\":true,\"renderData\":{\"expiredTime\":1000000000,"
      "\"certServiceType\":\"SYNTHETIC\",\"authMode\":\"ONLINE\","
      "\"renderType\":\"ONLINE\",\"qrCode\":\"1234\",\"config\":\"{}\"}}";
  assert(alipay_transit_update_card_data("NUMBER", "TYPE", error,
                                         sizeof(error)) == 5);
  alipay_transit_card_status_t status;
  memset(&status, 0xa5, sizeof(status));
  assert(alipay_transit_check_card_status("NUMBER", "TYPE", &status) == 0 &&
         status.is_exists && status.expire_timestamp == 2000000000 &&
         status.remain_use_count == 20);
  assert(h2_alipay_independent_release() == 0);
  assert(h2_alipay_independent_configure(&fixture.config, &fixture.sync.api) ==
         0);
  assert(alipay_pre_init() == 0 && alipay_init_transit_module() == 0);
  assert(alipay_transit_check_card_status("NUMBER", "TYPE", &status) == 0 &&
         status.is_exists && status.expire_timestamp == 2000000000);
  alipay_tansit_CardBaseVO_t last;
  memset(&last, 0xa5, sizeof(last));
  assert(alipay_transit_get_the_last_transcode_info(&last) == 4);
  assert(((uint8_t*)&last)[0] == 0xa5u);
  assert(h2_alipay_transit_discard(&fixture.config) == 0);
  assert(alipay_transit_check_card_status("NUMBER", "TYPE", &status) == 0 &&
         !status.is_exists);
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture.pref.api, "transit",
                          H2_PAL_PREF_OPEN_READ_ONLY, &namespace) == 0);
  void* factory = NULL;
  size_t factory_length = 0u;
  assert(namespace->get_blob(namespace, &fixture.memory.api,
                             "device_certificate", &factory,
                             &factory_length) == 0);
  assert(factory_length == 7u && memcmp(factory, "FACTORY", 7u) == 0);
  h2_pal_mem_free(&fixture.memory.api, factory);
  assert(namespace->close(namespace) == 0);
  finish(&fixture);
  h2_wolfcrypt_crypto_deinit();
  printf(
      "transit allocation failure points: list=%zu, code=%zu, native=%zu,"
      " sm2=%zu, yct=%zu, rct=%zu\n",
      list_faults, code_faults, offline_faults, sm2_faults, yct_faults,
      rct_faults);
  return 0;
}
