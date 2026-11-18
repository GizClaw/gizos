#include "h2_alipay_context.h"
#include "h2_alipay_client_internal.h"
#include "h2_alipay_api.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_time.h"
#include "h2_app_test_sync.h"
#include "h2_wolfcrypt_crypto.h"
#include "h2_sha256.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
  h2_app_test_mem_t mem;
  h2_app_test_pref_t pref;
  h2_app_test_time_t time;
  h2_app_test_sync_t sync;
  h2_pal_http_api_t http;
  h2_alipay_client_config_t config;
  const char* response;
  unsigned queries, acks, frees;
  int network_rc, ack_rc, rpc_status, http_status;
  uint32_t entropy;
  bool income_mode, break_during_request, ignore_cancel, parent_canceled;
} fixture_t;

static const char first_code[] = "284960766182755224";
static int entropy(void* user, uint8_t* data, size_t length) {
  uint32_t* state = user;
  for (size_t i = 0u; i < length; ++i) {
    *state = *state * 1664525u + 1013904223u;
    data[i] = (uint8_t)(*state >> 24u);
  }
  return 0;
}
static int http(void* user, const h2_pal_http_request_t* req,
                h2_pal_http_response_t* response) {
  fixture_t* f = user;
  const char* operation = NULL;
  size_t operation_length = 0u;
  for (size_t i = 0u; i < req->header_count; ++i) {
    const h2_pal_http_header_t* h = &req->headers[i];
    if (h->name.len == 14u &&
        memcmp(h->name.data, "Operation-Type", 14u) == 0) {
      operation = h->value.data;
      operation_length = h->value.len;
    }
    if (h->name.len == 4u && memcmp(h->name.data, "Sign", 4u) == 0)
      assert(h->value.len == 64u);
  }
  assert(operation != NULL);
  bool ack =
      operation_length >= 12u &&
      memcmp(operation + operation_length - 12u, "payResultAck", 12u) == 0;
  bool income =
      operation_length ==
          sizeof(
              "alipay.mobile.alideviceinfo.changecode.queryIncomeDetailByCode") -
              1u &&
      memcmp(operation,
             "alipay.mobile.alideviceinfo.changecode.queryIncomeDetailByCode",
             operation_length) == 0;
  assert(income == f->income_mode);
  assert(ack || income ||
         (operation_length >= 16u && memcmp(operation + operation_length - 16u,
                                            "queryOrderStatus", 16u) == 0));
  if (ack)
    ++f->acks;
  else
    ++f->queries;
  static const char query[] =
      "[{\"dynamicId\":\"284960766182755224\",\"userId\":\"TEST_UID\","
      "\"tid\":\"TEST_TID\",\"extInfo\":{\"sdkType\":\"INSIDE_WATCH\","
      "\"clientVersion\":\"4.61\",\"sdkHost\":\"H106\"}}]";
  static const char acknowledged[] =
      "[{\"dynamicId\":\"284960766182755224\",\"userId\":\"TEST_UID\","
      "\"tid\":\"TEST_TID\",\"extInfo\":{\"sdkType\":\"INSIDE_WATCH\","
      "\"ackType\":\"RPC\",\"clientVersion\":\"4.61\",\"sdkHost\":\"H106\"}}]";
  const char* expected = ack ? acknowledged : query;
  if (income) expected = "[{\"collectMoneyCode\":\"COLLECTION_TEST\"}]";
  assert(req->body_len == strlen(expected) &&
         memcmp(req->body, expected, req->body_len) == 0);
  if (f->network_rc != 0) return f->network_rc;
  if (ack && f->ack_rc != 0) return f->ack_rc;
  if (f->break_during_request) {
    alipay_query_flow_break();
    assert(req->cancel_cb != NULL && h2_pal_http_request_is_canceled(req));
    if (!f->ignore_cancel) return H2_PAL_ERR_CLOSED;
  }
  if (!f->ignore_cancel && h2_pal_http_request_is_canceled(req))
    return H2_PAL_ERR_CLOSED;
  const char* body = ack ? "{}" : f->response;
  size_t size = strlen(body);
  assert(size <= req->response_buf_cap);
  memcpy(req->response_buf, body, size);
  response->body = req->response_buf;
  response->body_len = size;
  response->status_code = f->http_status;
  char rpc[16];
  int length = snprintf(rpc, sizeof(rpc), "%d", f->rpc_status);
  assert(length > 0 && (size_t)length < sizeof(rpc));
  return h2_pal_http_deliver_response_header(req, "Result-Status", 13u, rpc,
                                             (size_t)length);
}
static void response_free(void* user, h2_pal_http_response_t* response) {
  ++((fixture_t*)user)->frees;
  h2_pal_http_response_reset(response);
}
static void hash(const uint8_t* data, size_t size, uint8_t out[32]) {
  h2_sha256_t h;
  h2_sha256_init(&h);
  h2_sha256_update(&h, data, size);
  h2_sha256_finish(&h, out);
}
static size_t encrypt(fixture_t* f, const uint8_t key[16], const uint8_t* input,
                      size_t size, uint8_t* output) {
  uint8_t iv[16], padded[144];
  assert(h2_alipay_paycode_iv(f->config.identity.mac, iv) == 0);
  size_t length = (size / 16u + 1u) * 16u;
  assert(length <= sizeof(padded));
  memcpy(padded, input, size);
  memset(padded + size, (int)(length - size), length - size);
  h2_pal_crypto_buf_t out = {.data = output, .cap = 144u};
  assert(h2_pal_crypto_aes_cbc(f->config.crypto, H2_PAL_CRYPTO_CIPHER_ENCRYPT,
                               key, 16u, iv, padded, length, &out) == 0 &&
         out.len == length);
  memset(padded, 0, sizeof(padded));
  return length;
}
static void store(fixture_t* f, const char* index, bool corrupt) {
  uint8_t data[1024] = {0}, middle[144], seed[144], key[32];
  char plain[96];
  for (unsigned i = 0u; i < 32u; ++i) key[i] = (uint8_t)i;
  int size =
      snprintf(plain, sizeof(plain),
               "%s###3132333435363738393031323334353637383930###60###", index);
  assert(size > 0 && (size_t)size < sizeof(plain));
  size_t first = encrypt(f, key, (const uint8_t*)plain, (size_t)size, middle);
  size_t length = encrypt(f, key + 16u, middle, first, seed);
  assert(length == 80u);
  if (corrupt) seed[length - 1u] ^= 1u;
  memcpy(data, "APB1", 4u);
  h2_alipay_client_fingerprint(&f->config, data + 4u);
  data[36] = BINDING_COMPLETE;
  data[39] = 8u;
  data[41] = 8u;
  data[47] = (uint8_t)length;
  size_t used = 49u;
  memcpy(data + used, "TEST_TID", 8u);
  used += 8u;
  memcpy(data + used, "TEST_UID", 8u);
  used += 8u;
  memcpy(data + used, seed, length);
  used += length;
  memcpy(data + used, key, 32u);
  used += 32u;
  hash(data, used, data + used);
  used += 32u;
  h2_pal_pref_namespace_t* ns = NULL;
  assert(h2_pal_pref_open(&f->pref.api, "payment", H2_PAL_PREF_OPEN_READ_WRITE,
                          &ns) == 0);
  assert(ns->set_blob(ns, "binding", data, used) == 0);
  memset(data, 0, sizeof(data));
  memcpy(data, "APC1", 4u);
  h2_alipay_client_fingerprint(&f->config, data + 4u);
  memcpy(data + 36u, "0123456789ABCDEF", 16u);
  data[52] = 7u;
  memcpy(data + 53u, "DEVICE1", 7u);
  assert(ns->set_blob(ns, "context", data, 60u) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
  memset(data, 0, sizeof(data));
  memset(key, 0, sizeof(key));
  memset(seed, 0, sizeof(seed));
  memset(middle, 0, sizeof(middle));
}
static void initialize(fixture_t* f) {
  memset(f, 0, sizeof(*f));
  h2_app_test_mem_init(&f->mem, NULL);
  assert(h2_app_test_pref_init(&f->pref, &f->mem.api) == 0);
  h2_app_test_time_init(&f->time, 0u);
  assert(h2_pal_time_set_wall_ms(&f->time.api, 59000u) == 0);
  h2_app_test_sync_init(&f->sync);
  const h2_wolfcrypt_crypto_config_t crypto = {&f->entropy, entropy};
  assert(h2_wolfcrypt_crypto_init(&crypto) == 0);
  static const h2_pal_http_vtable_t vt = {http, response_free};
  f->http = (h2_pal_http_api_t){f, &vt};
  f->config = (h2_alipay_client_config_t){
      .mem = &f->mem.api,
      .time = &f->time.api,
      .pref = &f->pref.api,
      .http = &f->http,
      .gateway_url = "https://iotapi.alipay.com/mgw.htm",
      .crypto = h2_wolfcrypt_crypto_api(),
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .pref_namespace = "payment",
      .product_type = 3u,
      .feature = 16385u,
      .compile_timestamp_s = 1794449000u};
  f->response = "{}";
  f->rpc_status = 1000;
  f->http_status = 200;
}
static void destroy(fixture_t* f) {
  assert(h2_alipay_independent_release() == 0);
  assert(f->pref.active_handles == 0u);
  assert(h2_app_test_pref_deinit(&f->pref) == 0);
  assert(f->mem.live_blocks == 0u);
  h2_wolfcrypt_crypto_deinit();
}
static void generation(fixture_t* f) {
  h2_alipay_client_payment_t result;
  uint8_t out[32];
  memset(out, 0xa5, sizeof(out));
  size_t length = sizeof(out);
  assert(h2_alipay_client_get_paycode(&f->config, out, &length, &result) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(out[0] == 0xa5 && f->queries == 0u);
  store(f, "1234567890", false);
  length = sizeof(out);
  int first_result =
      h2_alipay_client_get_paycode(&f->config, out, &length, &result);
  if (first_result != 0)
    fprintf(stderr, "generation rc=%d stage=%d\n", first_result,
            (int)result.stage);
  assert(first_result == 0);
  assert(length == 18u && strcmp((char*)out, first_code) == 0 &&
         out[19] == 0xa5);
  assert(f->queries == 0u && result.requests == 0u);
  for (size_t capacity = 0u; capacity < 19u; ++capacity) {
    memset(out, 0xa5, sizeof(out));
    length = capacity;
    assert(h2_alipay_client_get_paycode(&f->config, out, &length, &result) ==
           H2_PAL_ERR_NO_SPACE);
    assert(length == 18u && out[0] == 0xa5 && out[31] == 0xa5);
  }
  assert(h2_pal_time_set_wall_ms(&f->time.api, 60000u) == 0);
  length = sizeof(out);
  assert(h2_alipay_client_get_paycode(&f->config, out, &length, &result) == 0);
  assert(strcmp((char*)out, "289288341400287082") == 0);
  assert(h2_pal_time_set_wall_ms(&f->time.api, 59000u) == 0);
  store(f, "12345678901", false);
  length = sizeof(out);
  assert(h2_alipay_client_get_paycode(&f->config, out, &length, &result) == 0 &&
         length == 19u);
  store(f, "1234567890", true);
  length = sizeof(out);
  out[0] = 0xa5;
  assert(h2_alipay_client_get_paycode(&f->config, out, &length, &result) != 0 &&
         out[0] == 0xa5);
  store(f, "1234567890", false);
  f->mem.fail_at = f->mem.calls + 1u;
  length = sizeof(out);
  assert(h2_alipay_client_get_paycode(&f->config, out, &length, &result) ==
         H2_PAL_ERR_NO_MEMORY);
  f->mem.fail_at = 0u;
}
static void query(fixture_t* f) {
  const char* bodies[] = {
      "{}",
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":\"0.00\",\"originAmount\":\"1.00\"}}",
      "{\"entireData\":\"{\\\"action\\\":\\\"pay_success\\\",\\\"sdkBizData\\\":\\\"{\\\\\\\"originAmount\\\\\\\":\\\\\\\"2.34\\\\\\\"}\\\"}\"}",
      "{\"action\":\"CLOSECODE\"}",
      "{\"action\":\"failure_info\",\"sdkBizData\":\"limited\"}",
      "{\"action\":\"soundWavePaySuccessPush\",\"realAmount\":\"0\"}",
      "{\"action\":\"unsupported\"}",
  };
  const h2_alipay_client_payment_state_t expected[] = {
      H2_ALIPAY_CLIENT_PAYMENT_PENDING, H2_ALIPAY_CLIENT_PAYMENT_PAID,
      H2_ALIPAY_CLIENT_PAYMENT_PAID,    H2_ALIPAY_CLIENT_PAYMENT_DISABLED,
      H2_ALIPAY_CLIENT_PAYMENT_FAILED,  H2_ALIPAY_CLIENT_PAYMENT_PAID,
      H2_ALIPAY_CLIENT_PAYMENT_PENDING};
  const h2_alipay_client_payment_action_t actions[] = {
      H2_ALIPAY_CLIENT_ACTION_ABSENT,  H2_ALIPAY_CLIENT_ACTION_PAID,
      H2_ALIPAY_CLIENT_ACTION_PAID,    H2_ALIPAY_CLIENT_ACTION_DISABLED,
      H2_ALIPAY_CLIENT_ACTION_FAILURE, H2_ALIPAY_CLIENT_ACTION_SOUNDWAVE,
      H2_ALIPAY_CLIENT_ACTION_UNKNOWN};
  h2_alipay_client_payment_t result;
  char ext[128];
  for (size_t i = 0u; i < sizeof(bodies) / sizeof(*bodies); ++i) {
    f->response = bodies[i];
    size_t length = sizeof(ext);
    unsigned before = f->acks;
    assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                               ext, &length) == 0);
    assert(result.state == expected[i]);
    assert(!result.query.nfc_token_present && result.query.code_length == 18u &&
           result.query.tid_length == 8u && result.query.uid_length == 8u);
    assert(result.query.attempted && result.query.request_result == 0 &&
           result.query.http_status == 200 && result.query.rpc_status == 1000 &&
           result.query.body_length == strlen(bodies[i]) &&
           result.query.action == actions[i]);
    assert(f->acks - before ==
           (expected[i] == H2_ALIPAY_CLIENT_PAYMENT_PAID ? 1u : 0u));
    if (i == 1u) assert(strcmp(result.amount, "0.00") == 0);
    if (i == 2u) assert(strcmp(result.amount, "2.34") == 0);
    if (i == 4u) assert(length == 7u && memcmp(ext, "limited", 7u) == 0);
  }
  size_t length = 1u;
  ext[0] = (char)0xa5;
  f->response = bodies[4];
  assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                             ext,
                                             &length) == H2_PAL_ERR_NO_SPACE);
  assert(length == 7u && (unsigned char)ext[0] == 0xa5 &&
         result.state == H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN);
  f->response = bodies[1];
  f->ack_rc = H2_PAL_ERR_TIMEOUT;
  length = sizeof(ext);
  assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                             ext, &length) == 0);
  assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_PAID &&
         result.ack_result == H2_PAL_ERR_TIMEOUT);
  assert(result.query.request_result == 0 && result.query.http_status == 200 &&
         result.query.rpc_status == 1000 &&
         result.query.action == H2_ALIPAY_CLIENT_ACTION_PAID);
  f->ack_rc = 0;
  f->network_rc = H2_PAL_ERR_TIMEOUT;
  length = sizeof(ext);
  assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                             ext,
                                             &length) == H2_PAL_ERR_TIMEOUT);
  assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN &&
         result.amount[0] == '\0');
  f->network_rc = 0;
  f->response = "{invalid";
  length = sizeof(ext);
  assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                             ext,
                                             &length) == H2_PAL_ERR_FORMAT);
  f->response = bodies[1];
  f->rpc_status = 1001;
  length = sizeof(ext);
  assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                             ext, &length) != 0);
  assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN);
  f->rpc_status = 1000;
  f->response = "{}";
}
static void query_diagnostics(fixture_t* f) {
  h2_alipay_client_payment_t result;
  char ext[128];
  size_t length = sizeof(ext);
  unsigned acks = f->acks;
  f->response =
      "{\"action\":\"\\u0054EST_PENDING_0001\",\"success\":false,"
      "\"errorCode\":\"1007\",\"errorMsg\":\"synthetic\","
      "\"sdkBizData\":\"PRIVATE_FIXTURE\"}";
  assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                             ext, &length) == 0);
  uint8_t expected[32];
  static const uint8_t action[] = "TEST_PENDING_0001";
  hash(action, sizeof(action) - 1u, expected);
  assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_PENDING && f->acks == acks);
  assert(result.query.action == H2_ALIPAY_CLIENT_ACTION_UNKNOWN &&
         result.query.action_length == 17u && result.query.action_hash_valid &&
         result.query.action_identifier &&
         memcmp(result.query.action_sha256, expected, sizeof(expected)) == 0);
  assert(result.query.response_fields == (H2_ALIPAY_CLIENT_QUERY_SUCCESS_FALSE |
                                          H2_ALIPAY_CLIENT_QUERY_ERROR_CODE |
                                          H2_ALIPAY_CLIENT_QUERY_ERROR_MESSAGE |
                                          H2_ALIPAY_CLIENT_QUERY_BUSINESS));
  assert(
      result.query.response_code_valid && result.query.response_code == 1007 &&
      result.query.business_type == 2u && result.query.business_length == 15u);
  f->response = "{\"action\":\"TEST\\u0000STATE\"}";
  length = sizeof(ext);
  assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                             ext, &length) == 0);
  static const uint8_t binary_action[] = "TEST\0STATE";
  hash(binary_action, sizeof(binary_action) - 1u, expected);
  assert(result.query.action_hash_valid && !result.query.action_identifier &&
         result.query.action_length == sizeof(binary_action) - 1u &&
         memcmp(result.query.action_sha256, expected, sizeof(expected)) == 0);
  const char* codes[] = {"0",          "\"0012\"",       "-1",
                         "2147483648", "\"2147483648\"", "\"1007\\u0000x\"",
                         "true",       "\"TEXT_CODE\""};
  for (size_t i = 0u; i < sizeof(codes) / sizeof(codes[0]); ++i) {
    char body[128];
    (void)snprintf(body, sizeof(body), "{\"resultCode\":%s}", codes[i]);
    f->response = body;
    length = sizeof(ext);
    assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                               ext, &length) == 0);
    assert(!result.query.action_hash_valid && !result.query.action_identifier &&
           result.query.response_fields == H2_ALIPAY_CLIENT_QUERY_RESULT_CODE &&
           result.query.response_code_valid == (i < 2u) &&
           result.query.response_code == (i == 1u ? 12 : 0));
  }
  assert(f->acks == acks);
  f->response = "{}";
}

static void failures(fixture_t* f) {
  h2_alipay_client_payment_t result;
  char ext[128];
  const char* invalid[] = {
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":\"1234567890123456789012345678901234\"}}",
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":1}}",
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":\"1\\u00002\"}}",
      "{\"entireData\":\"bad json\"}",
      "{\"action\":false}",
  };
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(*invalid); ++i) {
    f->response = invalid[i];
    size_t length = sizeof(ext);
    memset(&result, 0xa5, sizeof(result));
    unsigned before = f->acks;
    assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                               ext, &length) != 0);
    assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN &&
           result.amount[0] == '\0');
    assert(f->acks == before && result.result != 0);
  }
  f->response =
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":\"0.00\"}}";
  size_t live = f->mem.live_blocks, calls = f->mem.calls, length = sizeof(ext);
  assert(h2_alipay_client_query_order_result(&f->config, first_code, &result,
                                             ext, &length) == 0);
  size_t count = f->mem.calls - calls;
  assert(count > 10u && f->mem.live_blocks == live);
  for (size_t n = 1u; n <= count; ++n) {
    f->mem.fail_at = f->mem.calls + n;
    length = sizeof(ext);
    int rc = h2_alipay_client_query_order_result(&f->config, first_code,
                                                 &result, ext, &length);
    f->mem.fail_at = 0u;
    assert(f->mem.live_blocks == live && f->pref.active_handles == 0u);
    if (rc != 0)
      assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN &&
             result.amount[0] == '\0');
    else
      assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_PAID);
    assert(result.result == rc);
  }
  result.state = H2_ALIPAY_CLIENT_PAYMENT_PAID;
  length = sizeof(ext);
  assert(h2_alipay_client_query_order_result(NULL, first_code, &result, ext,
                                             &length) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(result.state == H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN &&
         result.result == H2_PAL_ERR_INVALID_ARG);
  f->response = "{}";
}

static void income(fixture_t* fixture) {
  fixture->income_mode = true;
  fixture->response =
      "{\"success\":true,\"recordInfos\":[{\"amount\":\"1.23\",\"remark\":\"TEST\"}]}";
  h2_alipay_client_income_t output;
  memset(&output, 0x5a, sizeof(output));
  h2_alipay_client_payment_t result;
  size_t live = fixture->mem.live_blocks, calls = fixture->mem.calls;
  assert(h2_alipay_client_query_income(&fixture->config, "COLLECTION_TEST",
                                       &output, &result) == 0);
  size_t allocations = fixture->mem.calls - calls;
  assert(output.success && strcmp(output.message, "TEST") == 0);
  assert(output.bitmap[0][0] == 0x5a && fixture->mem.live_blocks == live);
  for (size_t offset = 1u; offset <= allocations; ++offset) {
    memset(&output, 0x5a, sizeof(output));
    fixture->mem.fail_at = fixture->mem.calls + offset;
    int rc = h2_alipay_client_query_income(&fixture->config, "COLLECTION_TEST",
                                           &output, &result);
    fixture->mem.fail_at = 0u;
    assert(rc != 0 && !output.success && output.message[0] == 0x5a);
    assert(fixture->mem.live_blocks == live &&
           fixture->pref.active_handles == 0u);
  }
  assert(h2_alipay_client_query_income(NULL, "COLLECTION_TEST", &output,
                                       &result) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_client_query_income(&fixture->config, NULL, &output,
                                       &result) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_client_query_income(&fixture->config, "COLLECTION_TEST",
                                       NULL,
                                       &result) == H2_PAL_ERR_INVALID_ARG);
  fixture->income_mode = false;
  fixture->response = "{}";
}

static int parent_cancel(void* user) {
  return ((fixture_t*)user)->parent_canceled;
}

static void abi(fixture_t* f) {
  retval_e (*generate)(uint8_t*, uint32_t*) = alipay_get_paycode;
  retval_e (*lookup)(char*, alipay_payment_status_e*, char*, char*, uint32_t*) =
      alipay_query_order_result_string;
  f->config.cancel_cb = parent_cancel;
  f->config.cancel_user = f;
  assert(h2_alipay_independent_configure(&f->config, &f->sync.api) == 0);
  assert(alipay_pre_init() == 0 && alipay_get_binding_status());
  struct {
    uint32_t before, length, after;
  } size = {0x12345678u, 32u, 0x87654321u};
  struct {
    uint8_t before, code[32], after;
  } code = {.before = 0xa5, .after = 0x5a};
  assert(generate(code.code, &size.length) == 0);
  assert(size.length == 18u && strcmp((char*)code.code, first_code) == 0);
  assert(size.before == 0x12345678u && size.after == 0x87654321u &&
         code.before == 0xa5 && code.after == 0x5a);
  static const char expected_collection[] =
      "CHANGECODE_SQ049607661827552240000000059";
  uint8_t collection_code[64];
  memset(collection_code, 0x5a, sizeof(collection_code));
  uint32_t collection_length = sizeof(collection_code);
  assert(alipay_get_collectioncode(collection_code, &collection_length) == 0);
  assert(collection_length == sizeof(expected_collection) - 1u);
  assert(strcmp((char*)collection_code, expected_collection) == 0);
  assert(collection_code[collection_length + 1u] == 0x5au);
  for (uint32_t capacity = 0u; capacity <= collection_length; ++capacity) {
    memset(collection_code, 0x5a, sizeof(collection_code));
    uint32_t length = capacity;
    assert(alipay_get_collectioncode(collection_code, &length) == 15);
    assert(length == capacity && collection_code[0] == 0x5au);
  }
  assert(alipay_get_collectioncode(NULL, &collection_length) == 19);
  assert(alipay_get_collectioncode(collection_code, NULL) == 19);
  struct {
    uint32_t before;
    alipay_payment_status_e status;
    uint32_t after;
  } status = {0x12345678u, ALIPAY_PAYMENT_STATUS_UNKNOWN, 0x87654321u};
  struct {
    char before, amount[32], after;
  } money = {.before = 'A', .after = 'Z'};
  char ext[128];
  size.length = sizeof(ext);
  f->response =
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":\"0.00\"}}";
  assert(lookup((char*)code.code, &status.status, money.amount, ext,
                &size.length) == 0);
  assert(status.status == ALIPAY_PAYMENT_STATUS_SUCCESS &&
         strcmp(money.amount, "0.00") == 0);
  assert(size.length == sizeof(ext));
  assert(status.before == 0x12345678u && status.after == 0x87654321u &&
         size.before == 0x12345678u && size.after == 0x87654321u &&
         money.before == 'A' && money.after == 'Z');
  f->response = "{}";
  size.length = sizeof(ext);
  assert(lookup((char*)code.code, &status.status, money.amount, ext,
                &size.length) == 0);
  assert(status.status == ALIPAY_PAYMENT_STATUS_NOTHING &&
         size.length == sizeof(ext));
  f->response =
      "{\"action\":\"pay_success\",\"sdkBizData\":{\"realAmount\":\"1.23\"}}";
  size_t live = f->mem.live_blocks;
  unsigned acknowledgements = f->acks;
  for (unsigned ignore = 0u; ignore < 2u; ++ignore) {
    char bitmap[32][14];
    memset(bitmap, 0x5a, sizeof(bitmap));
    f->break_during_request = true;
    f->ignore_cancel = ignore != 0u;
    assert(alipay_query_order_result((char*)code.code, &status.status,
                                     bitmap) == 7);
    assert(status.status == ALIPAY_PAYMENT_STATUS_UNKNOWN);
    assert(bitmap[0][0] == 0x5a && f->acks == acknowledgements);
    assert(f->mem.live_blocks == live && f->pref.active_handles == 0u);
  }
  f->break_during_request = f->ignore_cancel = false;
  f->parent_canceled = true;
  size.length = sizeof(ext);
  assert(lookup((char*)code.code, &status.status, money.amount, ext,
                &size.length) == 7);
  assert(status.status == ALIPAY_PAYMENT_STATUS_UNKNOWN &&
         f->acks == acknowledgements);
  f->parent_canceled = false;
  char bitmap[32][14] = {{0}};
  assert(alipay_query_order_result((char*)code.code, &status.status, bitmap) ==
         0);
  assert(status.status == ALIPAY_PAYMENT_STATUS_SUCCESS &&
         f->acks == acknowledgements + 1u);
  f->response = "{\"action\":\"failure_info\",\"sdkBizData\":\"limited\"}";
  size.length = 1u;
  assert(lookup((char*)code.code, &status.status, money.amount, ext,
                &size.length) != 0);
  assert(status.status == ALIPAY_PAYMENT_STATUS_UNKNOWN && size.length == 7u);
  store(f, "1234567890", true);
  size.length = sizeof(code.code);
  assert(generate(code.code, &size.length) == 99);
  store(f, "1234567890", false);
  f->response = "{\"action\":\"DELSEED\"}";
  size.length = sizeof(ext);
  assert(lookup((char*)code.code, &status.status, money.amount, ext,
                &size.length) == 0);
  assert(status.status == ALIPAY_PAYMENT_STATUS_UNBIND_BY_USER &&
         !alipay_get_binding_status());
  size.length = sizeof(code.code);
  assert(generate(code.code, &size.length) == 4);
}
int main(void) {
  fixture_t f;
  initialize(&f);
  generation(&f);
  query(&f);
  query_diagnostics(&f);
  failures(&f);
  income(&f);
  abi(&f);
  destroy(&f);
  return 0;
}
