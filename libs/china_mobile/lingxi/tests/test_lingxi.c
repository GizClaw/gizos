#include "h2_lingxi.h"
#include "h2_yyjson_json.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define INFO_RESPONSE                                          \
  "{\"code\":\"200\",\"success\":true,\"data\":{"              \
  "\"productId\":\"product-1\",\"deviceNo\":\"registered-1\"," \
  "\"deviceId\":\"device-1\",\"deviceSecret\":\"secret-1\"}}"

typedef struct fixture {
  h2_pal_mem_api_t mem;
  h2_pal_http_api_t http;
  h2_yyjson_json_t* json;
  h2_lingxi_config_t config;
  const char* response;
  const char* location;
  h2_pal_result_t transport_error;
  int status;
  bool report;
  bool inspect;
  bool oversize;
  bool foreign_buffer;
  size_t live;
  size_t allocations;
  size_t fail_at;
  size_t calls;
  size_t releases;
  const void* encoded;
  size_t encoded_len;
  const void* response_buffer;
  size_t response_capacity;
} fixture_t;

static void* allocate(void* user, size_t size) {
  fixture_t* f = user;
  ++f->allocations;
  if (f->allocations == f->fail_at) return NULL;
  void* result = malloc(size);
  if (result != NULL) ++f->live;
  return result;
}

static void* reallocate(void* user, void* ptr, size_t size) {
  fixture_t* f = user;
  if (ptr == NULL) return allocate(user, size);
  ++f->allocations;
  if (f->allocations == f->fail_at) return NULL;
  assert(size != 0u);
  return realloc(ptr, size);
}

static void assert_zero(const void* data, size_t len) {
  const unsigned char* bytes = data;
  for (size_t i = 0u; i < len; ++i) assert(bytes[i] == 0u);
}

static void release(void* user, void* ptr) {
  fixture_t* f = user;
  if (ptr == NULL) return;
  if (ptr == f->encoded) {
    assert_zero(ptr, f->encoded_len);
    f->encoded = NULL;
  }
  if (ptr == f->response_buffer) {
    assert_zero(ptr, f->response_capacity);
    f->response_buffer = NULL;
  }
  assert(f->live != 0u);
  --f->live;
  free(ptr);
}

static const h2_pal_mem_vtable_t mem_vtable = {
    .alloc = allocate, .realloc = reallocate, .free = release};

static h2_pal_json_value_t* get(fixture_t* f, h2_pal_json_value_t* object,
                                const char* key) {
  h2_pal_json_value_t* value = NULL;
  assert(h2_pal_json_object_get(h2_yyjson_json_api(f->json), object, key,
                                strlen(key), &value) == H2_PAL_OK);
  return value;
}

static void expect_string(fixture_t* f, h2_pal_json_value_t* object,
                          const char* key, const char* expected) {
  h2_pal_json_string_view_t text = {0};
  assert(h2_pal_json_value_get_string(h2_yyjson_json_api(f->json),
                                      get(f, object, key), &text) == H2_PAL_OK);
  assert(text.len == strlen(expected));
  assert(memcmp(text.data, expected, text.len) == 0);
}

static void inspect_request(fixture_t* f,
                            const h2_pal_http_request_t* request) {
  assert(request->method == H2_PAL_HTTP_POST);
  const char* path = f->report ? "/v2/customer/device/report"
                               : "/v2/customer/device/secret/info";
  const char* origin = "https://fixture.example:11443";
  assert(request->url.len == strlen(origin) + strlen(path));
  assert(memcmp(request->url.data, origin, strlen(origin)) == 0);
  assert(memcmp(request->url.data + strlen(origin), path, strlen(path)) == 0);
  assert(request->timeout_ms == 321);
  assert(request->retry_count == 0);
  assert(request->allocator == &f->mem);
  assert(strcmp(request->interface_name, "test-interface") == 0);
  assert(request->header_count == (f->config.source_ip != NULL ? 2u : 1u));
  assert(request->headers[0].value.len == 16u);
  assert(memcmp(request->headers[0].value.data, "application/json", 16u) == 0);
  if (f->config.source_ip != NULL) {
    assert(request->headers[1].name.len == 7u);
    assert(memcmp(request->headers[1].name.data, "X-AI-IP", 7u) == 0);
    assert(request->headers[1].value.len == strlen(f->config.source_ip));
  }
  const h2_pal_json_api_t* json = h2_yyjson_json_api(f->json);
  h2_pal_json_document_t* doc = NULL;
  h2_pal_json_value_t* root = NULL;
  assert(h2_pal_json_document_parse(json, request->body, request->body_len,
                                    NULL, &doc) == H2_PAL_OK);
  assert(h2_pal_json_document_root(json, doc, &root) == H2_PAL_OK);
  expect_string(f, root, "productId", "product-1");
  expect_string(f, root, "productKey", "key-\"\\\n-1");
  if (!f->report) {
    expect_string(f, root, "deviceNoType", "SN");
    expect_string(f, root, "deviceNo", "registered-1");
  } else {
    expect_string(f, root, "deviceId", "device-1");
    expect_string(f, root, "deviceSecret", "secret-1");
    h2_pal_json_value_t* params = get(f, root, "params");
    expect_string(f, params, "netSpeed", "100Mbps");
    expect_string(f, params, "netType", "wifi");
    expect_string(f, params, "platform", "fixture-platform");
    expect_string(f, params, "sdkVersion", "ai_fixture");
    expect_string(f, params, "firmwareVersion", "fixture-version");
    expect_string(f, params, "imei", "");
    expect_string(f, params, "cmei", "fixture-cmei");
    expect_string(f, params, "mac", "");
    h2_pal_json_value_t* ips = get(f, params, "innerIp");
    size_t count = 0u;
    assert(h2_pal_json_array_size(json, ips, &count) == H2_PAL_OK &&
           count == 2u);
    h2_pal_json_value_t* ip = NULL;
    h2_pal_json_string_view_t text = {0};
    assert(h2_pal_json_array_get(json, ips, 1u, &ip) == H2_PAL_OK);
    assert(h2_pal_json_value_get_string(json, ip, &text) == H2_PAL_OK);
    assert(text.len == 3u && memcmp(text.data, "::1", 3u) == 0);
  }
  assert(h2_pal_json_document_destroy(json, &doc) == H2_PAL_OK);
}

static h2_pal_result_t http_request(void* user,
                                    const h2_pal_http_request_t* request,
                                    h2_pal_http_response_t* response) {
  fixture_t* f = user;
  ++f->calls;
  f->encoded = request->body;
  f->encoded_len = request->body_len;
  f->response_buffer = request->response_buf;
  f->response_capacity = request->response_buf_cap;
  if (f->inspect) inspect_request(f, request);
  response->status_code = f->status;
  if (h2_pal_http_request_is_canceled(request)) return H2_PAL_ERR_CLOSED;
  if (f->location != NULL)
    return h2_pal_http_deliver_response_header(
        request, "lOcAtIoN", 8u, f->location, strlen(f->location));
  if (f->transport_error != H2_PAL_OK) return f->transport_error;
  const size_t len = strlen(f->response);
  if (len > request->response_buf_cap) return H2_PAL_ERR_NO_SPACE;
  memcpy(request->response_buf, f->response, len);
  response->body =
      f->foreign_buffer ? (uint8_t*)f->response : request->response_buf;
  response->body_len = f->oversize ? request->response_buf_cap + 1u : len;
  return H2_PAL_OK;
}

static void http_free(void* user, h2_pal_http_response_t* response) {
  fixture_t* f = user;
  ++f->releases;
  h2_pal_http_response_reset(response);
}

static const h2_pal_http_vtable_t http_vtable = {.request = http_request,
                                                 .response_free = http_free};

static void setup(fixture_t* f) {
  memset(f, 0, sizeof(*f));
  f->mem = (h2_pal_mem_api_t){.user = f, .vtable = &mem_vtable};
  f->http = (h2_pal_http_api_t){.user = f, .vtable = &http_vtable};
  assert(h2_yyjson_json_create(&f->mem, &f->json) == H2_PAL_OK);
  f->config = (h2_lingxi_config_t){
      .allocator = &f->mem,
      .http = &f->http,
      .json = h2_yyjson_json_api(f->json),
      .base_url = "https://fixture.example:11443/",
      .source_ip = "198.51.100.9",
      .interface_name = "test-interface",
      .json_limits = {4096u, 16u, 128u},
      .timeout_ms = 321u,
  };
  f->status = 200;
  f->response = INFO_RESPONSE;
  f->inspect = true;
}

static void teardown(fixture_t* f) {
  assert(h2_yyjson_json_destroy(&f->json) == H2_PAL_OK);
  assert(f->live == 0u);
}

static const h2_lingxi_product_t product = {"product-1", "key-\"\\\n-1"};
static const h2_lingxi_identity_t identity = {H2_LINGXI_DEVICE_NO_SN,
                                              "registered-1"};

static h2_pal_result_t fetch(fixture_t* f, h2_lingxi_device_info_t* info,
                             h2_lingxi_result_t* result) {
  memset(info, 0xa5, sizeof(*info));
  memset(result, 0xa5, sizeof(*result));
  return h2_lingxi_get_device_info(&f->config, &product, &identity, info,
                                   result);
}

static h2_lingxi_heartbeat_t sample_heartbeat(void) {
  static const char* const ips[] = {"192.0.2.1", "::1"};
  return (h2_lingxi_heartbeat_t){
      .inner_ips = ips,
      .inner_ip_count = 2u,
      .net_speed = "100Mbps",
      .net_type = "wifi",
      .platform = "fixture-platform",
      .sdk_version = "ai_fixture",
      .firmware_version = "fixture-version",
      .cmei = "fixture-cmei",
  };
}

static void test_roundtrip(void) {
  fixture_t f;
  setup(&f);
  const size_t baseline = f.live;
  h2_lingxi_device_info_t info;
  h2_lingxi_result_t result;
  assert(fetch(&f, &info, &result) == H2_PAL_OK);
  assert(result.http_status == 200 && result.platform_code == 200 &&
         result.platform_success);
  assert(strcmp(info.device_id, "device-1") == 0);
  assert(strcmp(info.device_secret, "secret-1") == 0);
  assert(f.live == baseline);
  h2_lingxi_heartbeat_t heartbeat = sample_heartbeat();
  f.report = true;
  f.config.source_ip = NULL;
  f.response = "{\"code\":200,\"success\":true}";
  assert(h2_lingxi_report_heartbeat(&f.config, &product, &info, &heartbeat,
                                    &result) == H2_PAL_OK);
  assert(f.live == baseline && f.calls == 2u && f.releases == 2u);
  heartbeat.sdk_version = "not-an-ai-sdk";
  assert(h2_lingxi_report_heartbeat(&f.config, &product, &info, &heartbeat,
                                    &result) == H2_PAL_ERR_INVALID_ARG);
  heartbeat.sdk_version = "ai_fixture";
  heartbeat.inner_ip_count = H2_LINGXI_MAX_INNER_IPS + 1u;
  assert(h2_lingxi_report_heartbeat(&f.config, &product, &info, &heartbeat,
                                    &result) == H2_PAL_ERR_INVALID_ARG);
  heartbeat.inner_ip_count = 2u;
  strcpy(info.product_id, "other-product");
  assert(h2_lingxi_report_heartbeat(&f.config, &product, &info, &heartbeat,
                                    &result) == H2_PAL_ERR_INVALID_ARG);
  assert(f.calls == 2u && result.http_status == 0 && !result.platform_success);
  h2_lingxi_device_info_clear(&info);
  assert_zero(&info, sizeof(info));
  h2_lingxi_device_info_clear(NULL);
  teardown(&f);
}

static void test_rejections(void) {
  static const char* const malformed[] = {
      "not json",
      "[]",
      "{}",
      "{\"code\":200}",
      "{\"code\":200,\"success\":\"true\"}",
      "{\"code\":true,\"success\":true}",
      "{\"code\":200.5,\"success\":true}",
      "{\"code\":-1,\"success\":true}",
      "{\"code\":2147483648,\"success\":true}",
      "{\"code\":\"2147483648\",\"success\":true}",
      "{\"code\":\"200x\",\"success\":true}",
      "{\"code\":200,\"success\":true,\"data\":{}}",
      "{\"code\":200,\"success\":true,\"data\":{\"productId\":\"other\",\"deviceNo\":\"registered-1\",\"deviceId\":\"d\",\"deviceSecret\":\"s\"}}",
      "{\"code\":200,\"success\":true,\"data\":{\"productId\":\"product-1\",\"deviceNo\":\"other\",\"deviceId\":\"d\",\"deviceSecret\":\"s\"}}",
      "{\"code\":200,\"success\":true,\"data\":{\"productId\":\"product-1\\u0000x\",\"deviceNo\":\"registered-1\",\"deviceId\":\"d\",\"deviceSecret\":\"s\"}}",
      "{\"code\":200,\"success\":true,\"data\":{\"productId\":\"product-1\",\"deviceNo\":\"registered-1\",\"deviceId\":\"\",\"deviceSecret\":\"s\"}}",
  };
  fixture_t f;
  setup(&f);
  const size_t baseline = f.live;
  h2_lingxi_device_info_t info;
  h2_lingxi_result_t result;
  for (size_t i = 0u; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
    f.response = malformed[i];
    assert(fetch(&f, &info, &result) == H2_PAL_ERR_FORMAT);
    assert_zero(&info, sizeof(info));
    assert(f.live == baseline);
  }
  f.response = "{\"code\":403,\"success\":false}";
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_UNAVAILABLE);
  assert(result.platform_code == 403 && !result.platform_success);
  f.response = INFO_RESPONSE;
  f.status = 401;
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_UNAVAILABLE);
  assert(result.http_status == 401 && !result.platform_success);
  f.status = 200;
  f.transport_error = -1000;
  assert(fetch(&f, &info, &result) == -1000);
  f.transport_error = H2_PAL_OK;
  f.location = "https://credential-trap.invalid/";
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_UNAVAILABLE);
  f.location = NULL;
  f.oversize = true;
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_FORMAT);
  f.oversize = false;
  f.foreign_buffer = true;
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_FORMAT);
  assert_zero(&info, sizeof(info));
  assert(f.live == baseline && f.calls == f.releases);
  teardown(&f);
}

static int cancel(void* user) { return *(int*)user; }

static void test_policy_and_budgets(void) {
  fixture_t f;
  setup(&f);
  f.inspect = false;
  h2_lingxi_device_info_t info;
  h2_lingxi_result_t result;
  const char* invalid[] = {"http://fixture.example", "https://",
                           "https://host/path",      "https://user@host",
                           "https://host?key=value", "https://host\r\nX:evil"};
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    f.config.base_url = invalid[i];
    assert(fetch(&f, &info, &result) == H2_PAL_ERR_INVALID_ARG);
    assert_zero(&info, sizeof(info));
  }
  assert(f.calls == 0u);
  f.config.allow_plaintext_loopback = true;
  f.config.base_url = "http://localhost.evil:80";
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_INVALID_ARG);
  f.config.base_url = "http://127.0.0.1:9000";
  assert(fetch(&f, &info, &result) == H2_PAL_OK);
  f.config.base_url = "http://[::1]:9000";
  assert(fetch(&f, &info, &result) == H2_PAL_OK);
  f.config.source_ip = "127.0.0.1\r\nX:evil";
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_INVALID_ARG);
  f.config.source_ip = NULL;
  int canceled = 1;
  f.config.cancel = cancel;
  f.config.cancel_user = &canceled;
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_CLOSED);
  f.config.cancel = NULL;
  f.config.json_limits.max_document_bytes = 64u;
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_NO_SPACE);
  f.config.json_limits.max_document_bytes = 4096u;
  f.config.json_limits.max_values = 2u;
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_NO_SPACE);
  f.config.json_limits.max_values = 128u;
  h2_pal_json_vtable_t incomplete = *f.config.json->vtable;
  incomplete.buffer_release = NULL;
  h2_pal_json_api_t json = {.user = f.config.json->user, .vtable = &incomplete};
  f.config.json = &json;
  assert(fetch(&f, &info, &result) == H2_PAL_ERR_INVALID_ARG);
  assert_zero(&info, sizeof(info));
  teardown(&f);
}

static void test_allocation_failures(void) {
  fixture_t f;
  setup(&f);
  f.inspect = false;
  h2_lingxi_device_info_t info;
  h2_lingxi_result_t result;
  size_t before = f.allocations;
  assert(fetch(&f, &info, &result) == H2_PAL_OK);
  const size_t count = f.allocations - before;
  const size_t baseline = f.live;
  for (size_t i = 1u; i <= count; ++i) {
    before = f.allocations;
    f.fail_at = before + i;
    const h2_pal_result_t rc = fetch(&f, &info, &result);
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_NO_MEMORY);
    if (rc != H2_PAL_OK) assert_zero(&info, sizeof(info));
    assert(f.live == baseline);
  }
  f.fail_at = 0u;
  assert(fetch(&f, &info, &result) == H2_PAL_OK);
  f.report = true;
  f.response = "{\"code\":200,\"success\":true}";
  const h2_lingxi_heartbeat_t heartbeat = sample_heartbeat();
  before = f.allocations;
  assert(h2_lingxi_report_heartbeat(&f.config, &product, &info, &heartbeat,
                                    &result) == H2_PAL_OK);
  const size_t report_count = f.allocations - before;
  for (size_t i = 1u; i <= report_count; ++i) {
    f.fail_at = f.allocations + i;
    const h2_pal_result_t rc = h2_lingxi_report_heartbeat(
        &f.config, &product, &info, &heartbeat, &result);
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_NO_MEMORY);
    assert(f.live == baseline);
  }
  f.fail_at = 0u;
  assert(h2_lingxi_report_heartbeat(&f.config, &product, &info, &heartbeat,
                                    &result) == H2_PAL_OK);
  teardown(&f);
}

static h2_pal_result_t root_failure(void* user, h2_pal_json_document_t* doc,
                                    h2_pal_json_value_t** out) {
  (void)user;
  (void)doc;
  *out = NULL;
  return -1000;
}

static h2_pal_result_t release_failure(void* user,
                                       h2_pal_json_buffer_t* buffer) {
  const h2_pal_result_t rc =
      h2_pal_json_buffer_release(h2_yyjson_json_api(user), buffer);
  assert(rc == H2_PAL_OK);
  return -1000;
}

static void test_provider_failures(void) {
  fixture_t f;
  setup(&f);
  h2_pal_json_vtable_t vtable = *f.config.json->vtable;
  h2_pal_json_api_t json = {.user = f.config.json->user, .vtable = &vtable};
  f.config.json = &json;
  h2_lingxi_device_info_t info;
  h2_lingxi_result_t result;
  vtable.document_root = root_failure;
  assert(fetch(&f, &info, &result) == -1000);
  assert_zero(&info, sizeof(info));
  vtable = *h2_yyjson_json_api(f.json)->vtable;
  vtable.buffer_release = release_failure;
  assert(fetch(&f, &info, &result) == -1000);
  assert_zero(&info, sizeof(info));
  teardown(&f);
}

int main(void) {
  test_roundtrip();
  test_rejections();
  test_policy_and_budgets();
  test_allocation_failures();
  test_provider_failures();
  return 0;
}
