#include "h2_alipay_e2e_independent.h"
#include "h2_alipay_test_binding.h"
#include "h2_alipay_api.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include "h2_wolfcrypt_crypto.h"
#include <assert.h>
#include <string.h>

typedef struct fixture {
  unsigned writes, requests;
  int write_rc, network_rc;
  const char* response;
  uint32_t entropy;
} fixture_t;

static int entropy(void* user, uint8_t* output, size_t length) {
  fixture_t* fixture = user;
  for (size_t index = 0u; index < length; ++index) {
    fixture->entropy = fixture->entropy * 1664525u + 1013904223u;
    output[index] = (uint8_t)(fixture->entropy >> 24u);
  }
  return 0;
}

static bool contains(const h2_pal_http_request_t* request, const char* text) {
  size_t size = strlen(text);
  if (size > request->body_len) return false;
  for (size_t i = 0u; i <= request->body_len - size; ++i)
    if (memcmp((const uint8_t*)request->body + i, text, size) == 0) return true;
  return false;
}

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  fixture_t* fixture = user;
  ++fixture->requests;
  for (size_t i = 0u; i < request->header_count; ++i) {
    const h2_pal_http_header_t* header = &request->headers[i];
    if (header->name.len == 14u &&
        memcmp(header->name.data, "Operation-Type", 14u) == 0 &&
        header->value.len >= 16u &&
        memcmp(header->value.data + header->value.len - 16u, "queryOrderStatus",
               16u) == 0) {
      assert(contains(request, "\"npayToken\":\"0123456789abcdef\""));
    }
  }
  assert(fixture->writes != 0u && fixture->write_rc == 0);
  if (fixture->network_rc != 0) return fixture->network_rc;
  size_t length = strlen(fixture->response);
  assert(length <= request->response_buf_cap);
  memcpy(request->response_buf, fixture->response, length);
  response->body = request->response_buf;
  response->body_len = length;
  response->status_code = 200;
  return h2_pal_http_deliver_response_header(request, "Result-Status", 13u,
                                             "1000", 4u);
}

static int write_reply(void* user, const uint8_t* data, size_t length) {
  fixture_t* fixture = user;
  assert(length > 5u && memcmp(data, "NW310", 5u) == 0);
  ++fixture->writes;
  return fixture->write_rc;
}

int main(void) {
  fixture_t fixture = {.entropy = 1u, .network_rc = H2_PAL_ERR_IO};
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &memory.api) == 0);
  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
  assert(h2_pal_time_set_wall_ms(&time.api, 1794449000000u) == 0);
  const h2_wolfcrypt_crypto_config_t crypto = {&fixture, entropy};
  assert(h2_wolfcrypt_crypto_init(&crypto) == 0);
  const h2_pal_http_vtable_t http_vtable = {.request = request};
  const h2_pal_http_api_t http = {&fixture, &http_vtable};
  const h2_alipay_client_config_t config = {
      .mem = &memory.api,
      .pref = &pref.api,
      .time = &time.api,
      .http = &http,
      .gateway_url = "https://iotapi.alipay.com/mgw.htm",
      .crypto = h2_wolfcrypt_crypto_api(),
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .pref_namespace = "payment_test",
      .product_type = 3u,
      .feature = 0x4005u,
      .compile_timestamp_s = 1794449000u};
  assert(h2_alipay_test_binding_seed(&config) == 0);
  const h2_alipay_payment_api_t* payment =
      h2_alipay_e2e_independent_payment_api();
  int unbind_sdk_rc;
  assert(payment->vtable->unbind != NULL);
  assert(payment->vtable->unbind(payment->user, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(payment->vtable->unbind(payment->user, &unbind_sdk_rc) ==
         H2_PAL_ERR_INVALID_STATE);
  const h2_alipay_tap_transport_t nfc = {.user = &fixture,
                                         .write_reply = write_reply};
  h2_alipay_payment_result_t result;
  assert(payment->vtable->start_tap(payment->user, &nfc, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_alipay_e2e_independent_configure_with_storage(
             &config, &sync.api, H2_ALIPAY_STORAGE_SESSION) == 0);
  const h2_alipay_api_t* api = h2_alipay_e2e_independent_api();
  bool first;
  int sdk_rc;
  assert(api->vtable->pre_init(api->user, &sdk_rc, &first) == 0);
  assert(first && sdk_rc == 0);
  assert(api->vtable->pre_init(api->user, &sdk_rc, &first) == 0);
  assert(!first && sdk_rc == 0);
  static const char url[] =
      "x?scheme=x%3FpageParams%3D%7B%22k%22%3A%22"
      "0123456789abcdef_SIG_a_b_c_d_3%22%7D";
  uint8_t input[5u + sizeof(url) - 1u] = {0xd1u, 1u, sizeof(url), 'U', 0u};
  memcpy(input + 5u, url, sizeof(url) - 1u);
  const uint8_t invalid_ndef[] = {0xd1u, 1u, 5u, 'U', 0u};
  assert(payment->vtable->start_tap(payment->user, &nfc, &result) == 0);
  assert(payment->vtable->receive_ndef(payment->user, invalid_ndef,
                                       sizeof(invalid_ndef),
                                       &result) == H2_PAL_ERR_IO);
  assert(result.processing_notified &&
         result.processing_status == NFC_PAYMENT_STATUS_FAIL &&
         !result.write_attempted && !result.response_sent &&
         fixture.writes == 0u && fixture.requests == 0u);
  assert(payment->vtable->stop_tap(payment->user) == 0);
  const int failures[] = {H2_PAL_ERR_IO, H2_PAL_ERR_BUSY, H2_PAL_ERR_TIMEOUT};
  for (size_t index = 0u; index < sizeof(failures) / sizeof(failures[0]);
       ++index) {
    fixture.write_rc = failures[index];
    assert(payment->vtable->start_tap(payment->user, &nfc, &result) == 0);
    assert(payment->vtable->unbind(payment->user, &unbind_sdk_rc) ==
           H2_PAL_ERR_BUSY);
    assert(payment->vtable->query_tap(payment->user, &result) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input),
                                         &result) == failures[index]);
    assert(result.sdk_rc == 0 && result.write_attempted &&
           !result.response_sent && result.nfc_write_rc == failures[index] &&
           result.transport_rc == failures[index] &&
           result.processing_notified &&
           result.processing_status == NFC_PAYMENT_STATUS_SUCCESS &&
           result.state != H2_ALIPAY_PAYMENT_PAID && result.amount[0] == '\0' &&
           fixture.writes == index + 1u && fixture.requests == 0u);
    assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input),
                                         &result) == H2_PAL_ERR_INVALID_STATE);
    assert(fixture.writes == index + 1u);
    assert(payment->vtable->query_tap(payment->user, &result) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(payment->vtable->stop_tap(payment->user) == 0);
  }
  fixture.write_rc = 0;
  assert(payment->vtable->start_tap(payment->user, &nfc, &result) == 0);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input),
                                       &result) == 0);
  assert(result.write_attempted && result.response_sent &&
         result.processing_notified &&
         result.processing_status == NFC_PAYMENT_STATUS_SUCCESS &&
         result.state == H2_ALIPAY_PAYMENT_PENDING &&
         result.amount[0] == '\0' && fixture.writes == 4u &&
         fixture.requests == 0u);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input),
                                       &result) == H2_PAL_ERR_INVALID_STATE);
  assert(fixture.writes == 4u);
  assert(payment->vtable->query_tap(payment->user, &result) == 0);
  assert(result.sdk_rc != 0 && result.state == H2_ALIPAY_PAYMENT_UNKNOWN &&
         result.amount[0] == '\0' && fixture.requests == 1u);
  assert(result.query_diagnostics_valid && result.query_attempted &&
         result.query_request_result == fixture.network_rc &&
         result.query_http_status == 0 && result.query_rpc_status == -1 &&
         result.query_action == H2_ALIPAY_CLIENT_ACTION_UNOBSERVED);
  fixture.network_rc = 0;
  fixture.response =
      "{\"action\":\"TEST_PENDING_0001\",\"success\":false,"
      "\"errorCode\":\"1007\",\"sdkBizData\":\"x\"}";
  assert(payment->vtable->query_tap(payment->user, &result) == 0);
  assert(result.sdk_rc == 0 && result.state == H2_ALIPAY_PAYMENT_PENDING &&
         result.amount[0] == '\0' && fixture.requests == 2u);
  assert(result.query_diagnostics_valid && result.query_attempted &&
         result.query_token_present && result.query_code_length >= 18u &&
         result.query_request_result == 0 && result.query_http_status == 200 &&
         result.query_rpc_status == 1000 &&
         result.query_body_length == strlen(fixture.response) &&
         result.query_action == H2_ALIPAY_CLIENT_ACTION_UNKNOWN);
  static const uint8_t expected_action_hash[32] = {
      0x65u, 0x9du, 0xd9u, 0xedu, 0x33u, 0xd8u, 0x8bu, 0x80u,
      0x40u, 0x6fu, 0xd8u, 0x50u, 0x4eu, 0x73u, 0xe0u, 0x5cu,
      0xc2u, 0x88u, 0x5du, 0xa4u, 0xf9u, 0x3bu, 0xa4u, 0x76u,
      0xd6u, 0x37u, 0x91u, 0xbfu, 0xccu, 0xd9u, 0x05u, 0x28u};
  assert(
      result.query_action_hash_valid && result.query_action_identifier &&
      memcmp(result.query_action_sha256, expected_action_hash,
             sizeof(expected_action_hash)) == 0 &&
      result.query_response_fields == 22u && result.query_business_type == 2u &&
      result.query_business_length == 1u && result.query_response_code_valid &&
      result.query_response_code == 1007);
  fixture.response =
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":\"0.00\"}}";
  assert(payment->vtable->query_tap(payment->user, &result) == 0);
  assert(result.sdk_rc == 0 && result.state == H2_ALIPAY_PAYMENT_PAID &&
         strcmp(result.amount, "0.00") == 0 && fixture.requests == 4u &&
         fixture.writes == 4u);
  assert(result.query_action == H2_ALIPAY_CLIENT_ACTION_PAID &&
         result.query_http_status == 200 && result.query_rpc_status == 1000 &&
         result.query_ack_result == 0);
  assert(payment->vtable->stop_tap(payment->user) == 0);
  pref.commit.result = H2_PAL_ERR_WRITE;
  pref.commit.remaining = 1u;
  assert(payment->vtable->unbind(payment->user, &unbind_sdk_rc) ==
         H2_PAL_ERR_IO);
  assert(unbind_sdk_rc != 0 && alipay_get_binding_status());
  assert(fixture.requests == 5u);
  fixture.network_rc = H2_PAL_ERR_TIMEOUT;
  assert(payment->vtable->unbind(payment->user, &unbind_sdk_rc) == 0);
  assert(unbind_sdk_rc == 0 && !alipay_get_binding_status());
  h2_alipay_binding_result_t binding;
  assert(api->vtable->binding_snapshot(api->user, &binding) == 0);
  assert(binding.state == H2_ALIPAY_BINDING_UNBOUND &&
         binding.tid_length == 0u && binding.uid_length == 0u &&
         binding.shared_key_length == 0u && binding.seed_length == 0u);
  uint8_t payer[H2_ALIPAY_PAYMENT_CODE_CAPACITY];
  uint32_t payer_length = sizeof(payer);
  assert(payment->vtable->get_code(payment->user, payer, &payer_length,
                                   &sdk_rc) == 0);
  assert(sdk_rc != 0 && payer_length == 0u);
  assert(fixture.requests == 6u);
  assert(h2_alipay_e2e_independent_release() == 0);
  assert(payment->vtable->receive_ndef(payment->user, input, sizeof(input),
                                       &result) == H2_PAL_ERR_INVALID_STATE);
  assert(pref.active_handles == 0u);
  assert(h2_app_test_pref_deinit(&pref) == 0 && memory.live_blocks == 0u);
  assert(fixture.requests == 6u);
  h2_wolfcrypt_crypto_deinit();
  return 0;
}
