#include "h2_alipay_scan.h"
#include "h2_alipay_tap.h"
#include "h2_alipay_client_internal.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_time.h"
#include "h2_sha256.h"

#include <assert.h>
#include <string.h>

typedef struct fixture {
  h2_app_test_mem_t memory;
  h2_app_test_pref_t pref;
  h2_app_test_time_t time;
  h2_pal_http_api_t http;
  h2_alipay_client_config_t config;
  h2_alipay_scan_t* scan;
  const char* reply;
  int network_result;
  unsigned requests, frees;
  bool union_mode;
  unsigned direct_payments, result_queries;
  const char* union_fault_operation;
  const char* union_fault_response;
} fixture_t;

typedef enum operation {
  PRECHECK,
  GENERATE,
  QUERY,
  CANCEL,
} operation_t;

static const char generation[] =
    "{\"success\":true,\"qrcode\":\"SYNTHETIC_CODE\"}";
static const char permission[] =
    "{\"success\":true,\"shopkeeperSweepDisplay\":true}";

static int request(void* user, const h2_pal_http_request_t* input,
                   h2_pal_http_response_t* output) {
  fixture_t* fixture = user;
  ++fixture->requests;
  if (fixture->network_result != 0) return fixture->network_result;
  const char* reply = fixture->reply;
  if (fixture->union_mode) {
    char operation[160] = {0};
    for (size_t index = 0u; index < input->header_count; ++index) {
      const h2_pal_http_header_t* header = &input->headers[index];
      if (header->name.len == 14u &&
          memcmp(header->name.data, "Operation-Type", 14u) == 0) {
        assert(header->value.len < sizeof(operation));
        memcpy(operation, header->value.data, header->value.len);
      }
    }
    if (strcmp(operation, "alipay.mobilecodec.route") == 0)
      reply =
          "{\"success\":true,\"routeInfos\":\"[{\\\"uri\\\":\\\""
          "alipays://platformapi/startapp?appId=20000123&"
          "biz_data=%7B%22u%22%3A%22PARENT_UID%22%7D\\\"}]\"}";
    else if (strstr(operation, "checkPaySameUser") != NULL)
      reply = "{\"success\":true,\"sameUser\":false}";
    else if (strstr(operation, "consult") != NULL)
      reply = "{\"success\":true,\"nickName\":\"NAME\",\"avatar\":\"LOGO\"}";
    else if (strstr(operation, "directPay") != NULL) {
      ++fixture->direct_payments;
      reply =
          "{\"success\":true,\"tradeNo\":\"TRADE\",\"resultCode\":\"INPROCESS\"}";
    } else {
      assert(strstr(operation, "payResult") != NULL);
      ++fixture->result_queries;
      reply = "{\"success\":true,\"resultCode\":\"SUCCESS\"}";
    }
    if (fixture->union_fault_operation != NULL &&
        strstr(operation, fixture->union_fault_operation) != NULL)
      reply = fixture->union_fault_response;
  }
  size_t length = strlen(reply);
  assert(length <= input->response_buf_cap);
  memcpy(input->response_buf, reply, length);
  output->body = input->response_buf;
  output->body_len = length;
  output->status_code = 200;
  return h2_pal_http_deliver_response_header(input, "Result-Status", 13u,
                                             "1000", 4u);
}

static int random_bytes(void* user, uint8_t* out, size_t length) {
  (void)user;
  memset(out, 0x42, length);
  return 0;
}

static void response_free(void* user, h2_pal_http_response_t* output) {
  ++((fixture_t*)user)->frees;
  h2_pal_http_response_reset(output);
}

static void seed(fixture_t* fixture) {
  h2_pal_pref_namespace_t* namespace = NULL;
  assert(h2_pal_pref_open(&fixture->pref.api, "scan",
                          H2_PAL_PREF_OPEN_READ_WRITE, &namespace) == 0);
  uint8_t data[160] = {0};
  memcpy(data, "APB1", 4u);
  h2_alipay_client_fingerprint(&fixture->config, data + 4u);
  data[36] = BINDING_COMPLETE;
  data[39] = data[41] = 8u;
  data[47] = 16u;
  memcpy(data + 49u, "TEST_TIDTEST_UID", 16u);
  h2_sha256_t hash;
  h2_sha256_init(&hash);
  h2_sha256_update(&hash, data, 113u);
  h2_sha256_finish(&hash, data + 113u);
  assert(namespace->set_blob(namespace, "binding", data, 145u) == 0);
  memset(data, 0, sizeof(data));
  memcpy(data, "APC1", 4u);
  h2_alipay_client_fingerprint(&fixture->config, data + 4u);
  memcpy(data + 36u, "0123456789ABCDEF", 16u);
  data[52] = 7u;
  memcpy(data + 53u, "DEVICE1", 7u);
  assert(namespace->set_blob(namespace, "context", data, 60u) == 0);
  assert(namespace->commit(namespace) == 0 && namespace->close(namespace) == 0);
}

static void initialize(fixture_t* fixture) {
  memset(fixture, 0, sizeof(*fixture));
  h2_app_test_mem_init(&fixture->memory, NULL);
  assert(h2_app_test_pref_init(&fixture->pref, &fixture->memory.api) == 0);
  h2_app_test_time_init(&fixture->time, 0u);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  static const h2_pal_http_vtable_t http = {request, response_free};
  static const h2_pal_crypto_vtable_t crypto_vtable = {.random = random_bytes};
  static const h2_pal_crypto_api_t crypto = {.vtable = &crypto_vtable};
  fixture->http = (h2_pal_http_api_t){fixture, &http};
  fixture->config = (h2_alipay_client_config_t){
      .mem = &fixture->memory.api,
      .pref = &fixture->pref.api,
      .time = &fixture->time.api,
      .http = &fixture->http,
      .gateway_url = "https://iotapi.alipay.com/mgw.htm",
      .crypto = &crypto,
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .pref_namespace = "scan",
      .product_type = 3u,
      .feature = 16385u,
      .compile_timestamp_s = 1794449000u};
  fixture->reply = permission;
  seed(fixture);
  assert(h2_alipay_scan_open(&fixture->config, &fixture->scan) == 0);
}

static void prepare(fixture_t* fixture, operation_t operation) {
  fixture->network_result = 0;
  if (operation == QUERY || operation == CANCEL) {
    fixture->reply = generation;
    h2_alipay_scan_code_t code;
    h2_alipay_client_payment_t result;
    assert(h2_alipay_scan_dzs_generate(fixture->scan, "0.01", &code, &result) ==
           0);
    assert(code.success && strcmp(code.code, "SYNTHETIC_CODE") == 0);
  }
  fixture->reply = operation == PRECHECK ? permission
                   : operation == GENERATE
                       ? generation
                       : "{\"success\":true,\"resultCode\":\"INPROCESS\"}";
}

static int invoke(fixture_t* fixture, operation_t operation,
                  h2_alipay_client_payment_t* result) {
  uint8_t enabled = 0xffu;
  h2_alipay_scan_code_t code;
  if (operation == PRECHECK)
    return h2_alipay_scan_dzs_precheck(fixture->scan, &enabled, result);
  if (operation == GENERATE)
    return h2_alipay_scan_dzs_generate(fixture->scan, "0.01", &code, result);
  if (operation == QUERY)
    return h2_alipay_scan_dzs_query(fixture->scan, result);
  return h2_alipay_scan_dzs_cancel(fixture->scan, result);
}

static void allocation_failures(fixture_t* fixture, operation_t operation) {
  prepare(fixture, operation);
  size_t live = fixture->memory.live_blocks, first = fixture->memory.calls;
  h2_alipay_client_payment_t result;
  assert(invoke(fixture, operation, &result) == 0);
  size_t count = fixture->memory.calls - first;
  assert(count > 5u && fixture->memory.live_blocks == live);
  for (size_t offset = 1u; offset <= count; ++offset) {
    prepare(fixture, operation);
    fixture->memory.fail_at = fixture->memory.calls + offset;
    int rc = invoke(fixture, operation, &result);
    fixture->memory.fail_at = 0u;
    assert(rc != 0 && result.result == rc);
    assert(fixture->memory.live_blocks == live);
    assert(fixture->pref.active_handles == 0u);
    if (operation == CANCEL && result.requests == 0u) {
      fixture->reply = "{\"success\":true,\"resultCode\":\"INPROCESS\"}";
      assert(h2_alipay_scan_dzs_query(fixture->scan, &result) == 0);
      assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_PROCESSING);
    }
  }
}

static void behavior(fixture_t* fixture) {
  h2_alipay_scan_code_t code;
  h2_alipay_client_payment_t result;
  uint8_t enabled = 0xffu;
  fixture->reply = permission;
  assert(h2_alipay_scan_dzs_precheck(fixture->scan, &enabled, &result) == 0);
  assert(enabled == 1u && result.requests == 1u);
  prepare(fixture, QUERY);
  unsigned requests = fixture->requests;
  assert(h2_alipay_scan_dzs_query(fixture->scan, &result) == 0);
  assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_PROCESSING);
  assert(fixture->requests == requests + 1u);
  fixture->network_result = H2_PAL_ERR_IO;
  assert(h2_alipay_scan_dzs_cancel(fixture->scan, &result) == H2_PAL_ERR_IO);
  fixture->network_result = 0;
  requests = fixture->requests;
  assert(h2_alipay_scan_dzs_query(fixture->scan, &result) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_alipay_scan_dzs_cancel(fixture->scan, &result) == 0);
  assert(fixture->requests == requests);
  static const char* malformed[] = {
      "{\"success\":true,\"success\":false,\"qrcode\":\"CODE\"}",
      "{\"success\":true,\"qrcode\":123}",
      "{\"success\":true,\"qrcode\":\"A\\u0000B\"}",
      "{\"success\":false}",
      "[]",
      "{"};
  for (size_t index = 0u; index < sizeof(malformed) / sizeof(*malformed);
       ++index) {
    fixture->reply = malformed[index];
    assert(h2_alipay_scan_dzs_generate(fixture->scan, "0.01", &code, &result) ==
           H2_PAL_ERR_FORMAT);
    assert(!code.success && code.length == 0u);
  }
  fixture->reply = generation;
  assert(h2_pal_time_set_wall_ms(&fixture->time.api,
                                 (UINT64_C(4294967296)) * 1000u) == 0);
  requests = fixture->requests;
  assert(h2_alipay_scan_dzs_generate(fixture->scan, "0.01", &code, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture->requests == requests);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) == 0);
  assert(h2_alipay_scan_dzs_generate(fixture->scan, NULL, &code, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_scan_dzs_query(NULL, &result) == H2_PAL_ERR_INVALID_ARG);
}

static void union_behavior(fixture_t* fixture) {
  fixture->union_mode = true;
  h2_alipay_union_t* session = NULL;
  assert(h2_alipay_union_open(&fixture->config, &session) == 0);
  h2_alipay_scan_order_t order;
  memset(&order, 0xa5, sizeof(order));
  h2_alipay_client_payment_t result;
  unsigned requests = fixture->requests;
  assert(h2_alipay_union_parse(session, "https://invalid/no-query", true,
                               &order, &result) == 0);
  assert(!order.success && strcmp(order.error, "-1") == 0);
  assert((uint8_t)order.name[0] == 0xa5u && fixture->requests == requests);
  assert(h2_alipay_union_parse(session, "CODE", false, &order, &result) == 0);
  assert(order.success && order.amount[0] == '\0' && result.requests == 3u);
  strcpy(order.amount, "0.01");
  h2_alipay_scan_failure_t failure;
  unsigned payments = fixture->direct_payments;
  assert(h2_alipay_union_confirm(session, &order, &failure, &result) == 0);
  assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_PAID &&
         result.requests == 2u);
  assert(fixture->direct_payments == payments + 1u);
  assert(h2_alipay_union_confirm(session, &order, &failure, &result) == 0);
  assert(result.requests == 1u && fixture->direct_payments == payments + 1u);
  strcpy(order.amount, "0.02");
  requests = fixture->requests;
  assert(h2_alipay_union_confirm(session, &order, &failure, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(fixture->requests == requests);
  fixture->union_fault_operation = "directPay";
  fixture->union_fault_response = "{";
  assert(h2_alipay_union_parse(session, "CODE", false, &order, &result) == 0);
  strcpy(order.amount, "0.01");
  assert(h2_alipay_union_confirm(session, &order, &failure, &result) ==
         H2_PAL_ERR_FORMAT);
  payments = fixture->direct_payments;
  fixture->union_fault_operation = NULL;
  assert(h2_alipay_union_confirm(session, &order, &failure, &result) == 0);
  assert(result.requests == 1u && fixture->direct_payments == payments);
  fixture->union_fault_operation = "consult";
  fixture->union_fault_response =
      "{\"success\":true,\"nickName\":\"A\\u0000B\"}";
  assert(h2_alipay_union_parse(session, "CODE", false, &order, &result) ==
         H2_PAL_ERR_FORMAT);
  requests = fixture->requests;
  strcpy(order.amount, "0.01");
  assert(h2_alipay_union_confirm(session, &order, &failure, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(fixture->requests == requests);
  fixture->union_fault_operation = NULL;
  assert(h2_alipay_union_parse(session, "CODE", false, &order, &result) == 0);
  size_t first = fixture->memory.calls;
  size_t live = fixture->memory.live_blocks;
  assert(h2_alipay_union_parse(session, "CODE", false, &order, &result) == 0);
  size_t count = fixture->memory.calls - first;
  assert(count > 10u);
  for (size_t offset = 1u; offset <= count; ++offset) {
    fixture->memory.fail_at = fixture->memory.calls + offset;
    assert(h2_alipay_union_parse(session, "CODE", false, &order, &result) != 0);
    fixture->memory.fail_at = 0u;
    assert(fixture->memory.live_blocks == live &&
           fixture->pref.active_handles == 0u);
  }
  assert(h2_alipay_union_parse(session, "CODE", false, &order, &result) == 0);
  strcpy(order.amount, "0.01");
  first = fixture->memory.calls;
  assert(h2_alipay_union_confirm(session, &order, &failure, &result) == 0);
  count = fixture->memory.calls - first;
  assert(count > 10u);
  for (size_t offset = 1u; offset <= count; ++offset) {
    assert(h2_alipay_union_parse(session, "CODE", false, &order, &result) == 0);
    strcpy(order.amount, "0.01");
    fixture->memory.fail_at = fixture->memory.calls + offset;
    assert(h2_alipay_union_confirm(session, &order, &failure, &result) != 0);
    fixture->memory.fail_at = 0u;
    assert(fixture->memory.live_blocks == live &&
           fixture->pref.active_handles == 0u);
  }
  h2_alipay_union_close(session);
  h2_alipay_union_close(NULL);
  fixture->union_mode = false;
}

static void tap_behavior(fixture_t* fixture) {
  h2_alipay_tap_t* tap = NULL;
  assert(h2_alipay_tap_open(&fixture->config, &tap) == 0);
  uint8_t output[32];
  memset(output, 0xa5, sizeof(output));
  size_t length = 0xa5u;
  assert(h2_alipay_tap_plain_code(tap, (const uint8_t*)"TOKEN", 5u, output,
                                  sizeof(output),
                                  &length) == H2_PAL_ERR_NOT_FOUND);
  assert(output[0] == 0xa5u && length == 0xa5u);
  assert(h2_alipay_tap_set_context(tap, "TOKEN", "SQUEEZE",
                                   "281234567890123456") == 0);
  assert(h2_alipay_tap_plain_code(tap, (const uint8_t*)"TOKEN", 5u, output, 17u,
                                  &length) == H2_PAL_ERR_NO_SPACE);
  assert(output[0] == 0xa5u && length == 0xa5u);
  assert(h2_alipay_tap_set_context(tap, "TOKEN", "", "CODE") ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_tap_plain_code(tap, (const uint8_t*)"TOKEN", 5u, output, 18u,
                                  &length) == 0);
  assert(length == 18u && output[18] == 0xa5u);
  uint8_t written = 2u;
  assert(h2_alipay_tap_write_result(tap, (const uint8_t*)"TOKEN", 5u, true,
                                    &written) == 0);
  written = 0xa5u;
  assert(h2_alipay_tap_write_result(tap, (const uint8_t*)"OTHER", 5u, false,
                                    &written) == H2_PAL_ERR_NOT_FOUND);
  assert(written == 0xa5u);
  assert(h2_alipay_tap_write_result(tap, (const uint8_t*)"TOKEN", 5u, false,
                                    &written) == 0 &&
         written == 2u);
  assert(h2_alipay_tap_set_context(tap, "TOKEN", "", "CODE") ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_tap_write_result(tap, (const uint8_t*)"TOKEN", 5u, false,
                                    &written) == 0 &&
         written == 2u);
  assert(h2_alipay_tap_set_context(tap, "TOKEN", "SQUEEZE",
                                   "281234567890123456") == 0);
  assert(h2_alipay_tap_write_result(tap, (const uint8_t*)"TOKEN", 5u, false,
                                    &written) == 0 &&
         written == 0u);
  uint8_t packet[27] = {1u};
  h2_alipay_tap_payment_t payment;
  memset(&payment, 0xa5, sizeof(payment));
  assert(h2_alipay_tap_parse_result(tap, packet, 27u, &payment) ==
         H2_PAL_ERR_UNSUPPORTED);
  h2_alipay_tap_payment_t zero = {0};
  assert(memcmp(&payment, &zero, sizeof(payment)) == 0);
  fixture->pref.read =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  assert(h2_alipay_tap_parse_result(tap, packet, 27u, &payment) ==
         H2_PAL_ERR_IO);
  h2_alipay_tap_close(tap);
  h2_alipay_tap_close(NULL);
}

static void tap_tags(fixture_t* fixture) {
  static const uint8_t url[] =
      "x?scheme=x%3FpageParams%3D%7B%22k%22%3A%22"
      "0123456789abcdef_SIG_a_b_c_d_0%22%7D";
  h2_alipay_tap_tag_t tag;
  size_t baseline = fixture->memory.live_blocks;
  assert(h2_alipay_tap_decode_tag(&fixture->config, url, sizeof(url) - 1u,
                                  &tag) == 0);
  assert(tag.type == 2u && tag.flags_present && tag.trusted);
  assert(strcmp(tag.token, "0123456789abcdef") == 0);
  assert(fixture->memory.live_blocks == baseline);
  for (size_t fail = 1u; fail < 64u; ++fail) {
    size_t before = fixture->memory.calls;
    fixture->memory.fail_at = before + fail;
    int rc =
        h2_alipay_tap_decode_tag(&fixture->config, url, sizeof(url) - 1u, &tag);
    assert(fixture->memory.live_blocks == baseline);
    bool reached = fixture->memory.calls >= before + fail;
    fixture->memory.fail_at = 0u;
    if (!reached) {
      assert(rc == 0);
      break;
    }
    assert(rc == H2_PAL_ERR_NO_MEMORY);
    assert(fail != 63u);
  }
  uint8_t record[256] = {0xd1u, 1u, sizeof(url), 'U', 4u};
  memcpy(record + 5u, url, sizeof(url) - 1u);
  assert(h2_alipay_tap_decode_tag(&fixture->config, record, sizeof(url) + 4u,
                                  &tag) == 0);
  for (size_t length = 0u; length < sizeof(url) + 4u; ++length) {
    assert(h2_alipay_tap_decode_tag(&fixture->config, record, length, &tag) !=
           0);
    assert(fixture->memory.live_blocks == baseline);
  }
  memset(record, 0, sizeof(record));
  record[0] = 0xc9u;
  record[1] = 1u;
  record[5] = sizeof(url);
  record[6] = 2u;
  record[7] = 'U';
  record[8] = 'I';
  record[9] = 'D';
  record[10] = 4u;
  memcpy(record + 11u, url, sizeof(url) - 1u);
  assert(h2_alipay_tap_decode_tag(&fixture->config, record, sizeof(url) + 10u,
                                  &tag) == 0);
  assert(tag.type == 2u && strcmp(tag.token, "0123456789abcdef") == 0);
  memset(record, 0, sizeof(record));
  const uint8_t first[] = {0x91u, 1u, 1u, 'T', 0u};
  memcpy(record, first, sizeof(first));
  record[5] = 0x51u;
  record[6] = 1u;
  record[7] = sizeof(url);
  record[8] = 'U';
  record[9] = 4u;
  memcpy(record + 10u, url, sizeof(url) - 1u);
  assert(h2_alipay_tap_decode_tag(&fixture->config, record, sizeof(url) + 9u,
                                  &tag) == 0);
  assert(tag.type == 2u && tag.flags_present && tag.trusted);
  uint8_t oversized[] = {0xc1u, 1u, 255u, 255u, 255u, 255u, 'U', 4u};
  assert(h2_alipay_tap_decode_tag(&fixture->config, oversized,
                                  sizeof(oversized),
                                  &tag) == H2_PAL_ERR_FORMAT);
  assert(h2_alipay_tap_decode_tag(NULL, url, sizeof(url) - 1u, &tag) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(fixture->memory.live_blocks == baseline);
}

int main(void) {
  fixture_t fixture;
  initialize(&fixture);
  behavior(&fixture);
  union_behavior(&fixture);
  tap_behavior(&fixture);
  tap_tags(&fixture);
  for (unsigned operation = PRECHECK; operation <= CANCEL; ++operation)
    allocation_failures(&fixture, (operation_t)operation);
  h2_alipay_scan_close(fixture.scan);
  assert(fixture.pref.active_handles == 0u);
  assert(h2_app_test_pref_deinit(&fixture.pref) == 0);
  assert(fixture.memory.live_blocks == 0u);
  h2_alipay_scan_close(NULL);
  return 0;
}
