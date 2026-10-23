#include "h2_lingxi_e2e.h"
#include "h2_yyjson_json.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  h2_pal_mem_api_t mem;
  h2_pal_http_api_t http;
  h2_pal_time_api_t time;
  h2_yyjson_json_t* json;
  h2_lingxi_e2e_config_t config;
  size_t live;
  unsigned requests;
  unsigned reports;
  bool query_rejected;
  bool heartbeat_rejected;
  h2_pal_result_t clock_error;
  uint64_t now;
  uint64_t step;
} fixture_t;

static void* allocate(void* user, size_t size) {
  fixture_t* f = user;
  void* ptr = malloc(size);
  if (ptr != NULL) ++f->live;
  return ptr;
}

static void* resize(void* user, void* ptr, size_t size) {
  if (ptr == NULL) return allocate(user, size);
  assert(size != 0u);
  return realloc(ptr, size);
}

static void release(void* user, void* ptr) {
  fixture_t* f = user;
  if (ptr == NULL) return;
  assert(f->live != 0u);
  --f->live;
  free(ptr);
}

static h2_pal_result_t now(void* user, uint64_t* value) {
  fixture_t* f = user;
  f->now += f->step;
  *value = f->now;
  return f->clock_error;
}

static h2_pal_result_t request(void* user, const h2_pal_http_request_t* req,
                               h2_pal_http_response_t* response) {
  fixture_t* f = user;
  ++f->requests;
  assert(req->method == H2_PAL_HTTP_POST && req->retry_count == 0);
  assert(req->timeout_ms == 100);
  const bool query = f->requests == 1u;
  const bool reject = query ? f->query_rejected : f->heartbeat_rejected;
  const char* text =
      query
          ? "{\"code\":200,\"success\":true,\"data\":{\"productId\":\"p\",\"deviceNo\":\"n\",\"deviceId\":\"d\",\"deviceSecret\":\"s\"}}"
          : "{\"code\":200,\"success\":true}";
  if (reject) text = "{\"code\":403,\"success\":false}";
  assert(strlen(text) < req->response_buf_cap);
  memcpy(req->response_buf, text, strlen(text));
  response->body = req->response_buf;
  response->body_len = strlen(text);
  response->status_code = 200;
  return H2_PAL_OK;
}

static void response_free(void* user, h2_pal_http_response_t* response) {
  (void)user;
  h2_pal_http_response_reset(response);
}

static void report(void* user, const h2_lingxi_e2e_case_result_t* row) {
  fixture_t* f = user;
  static const char* const expected[] = {"get-device-info", "heartbeat",
                                         "credential-cleanup"};
  assert(f->reports < H2_LINGXI_E2E_CASE_COUNT);
  assert(strcmp(row->id, expected[f->reports]) == 0);
  ++f->reports;
}

static const h2_pal_mem_vtable_t mem_vtable = {allocate, resize, release};
static const h2_pal_http_vtable_t http_vtable = {request, response_free};
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = now};

static void setup(fixture_t* f) {
  memset(f, 0, sizeof(*f));
  f->mem = (h2_pal_mem_api_t){f, &mem_vtable};
  f->http = (h2_pal_http_api_t){f, &http_vtable};
  f->time = (h2_pal_time_api_t){f, &time_vtable};
  assert(h2_yyjson_json_create(&f->mem, &f->json) == H2_PAL_OK);
  f->step = 5u;
  static const char* const ips[] = {"127.0.0.1"};
  f->config = (h2_lingxi_e2e_config_t){
      .client = {.allocator = &f->mem,
                 .http = &f->http,
                 .json = h2_yyjson_json_api(f->json),
                 .base_url = "https://fixture.example",
                 .json_limits = {4096u, 16u, 128u},
                 .timeout_ms = 100u},
      .time = &f->time,
      .product = {"p", "k"},
      .identity = {H2_LINGXI_DEVICE_NO_SN, "n"},
      .heartbeat = {.inner_ips = ips,
                    .inner_ip_count = 1u,
                    .net_speed = "0Mbps",
                    .net_type = "wifi",
                    .platform = "fixture",
                    .sdk_version = "ai_http_1.0",
                    .firmware_version = "host-test"},
      .report = report,
      .report_user = f,
  };
}

static void teardown(fixture_t* f) {
  assert(f->reports == H2_LINGXI_E2E_CASE_COUNT);
  assert(h2_yyjson_json_destroy(&f->json) == H2_PAL_OK);
  assert(f->live == 0u);
}

int main(void) {
  fixture_t f;
  h2_lingxi_e2e_result_t result;
  setup(&f);
  const size_t baseline = f.live;
  assert(h2_lingxi_e2e_run(&f.config, &result) == H2_PAL_OK);
  assert(result.passed == H2_LINGXI_E2E_CASE_COUNT && result.failed == 0u &&
         result.blocked == 0u);
  assert(f.requests == 2u && f.live == baseline);
  teardown(&f);

  setup(&f);
  f.query_rejected = true;
  assert(h2_lingxi_e2e_run(&f.config, &result) == H2_PAL_ERR_UNAVAILABLE);
  assert(result.failed == 1u && result.blocked == 1u && result.passed == 1u);
  assert(result.cases[H2_LINGXI_E2E_HEARTBEAT].status == H2_LINGXI_E2E_BLOCKED);
  assert(result.cases[H2_LINGXI_E2E_CREDENTIAL_CLEANUP].status ==
         H2_LINGXI_E2E_PASS);
  assert(f.requests == 1u);
  teardown(&f);

  setup(&f);
  f.heartbeat_rejected = true;
  assert(h2_lingxi_e2e_run(&f.config, &result) == H2_PAL_ERR_UNAVAILABLE);
  assert(result.failed == 1u && result.passed == 2u && result.blocked == 0u);
  assert(f.requests == 2u);
  teardown(&f);

  setup(&f);
  f.config.time = NULL;
  assert(h2_lingxi_e2e_run(&f.config, &result) == H2_PAL_ERR_INVALID_ARG);
  assert(f.requests == 0u && result.blocked == H2_LINGXI_E2E_CASE_COUNT);
  teardown(&f);

  setup(&f);
  f.clock_error = -1000;
  assert(h2_lingxi_e2e_run(&f.config, &result) == -1000);
  assert(f.requests == 0u && result.failed == 1u && result.blocked == 1u &&
         result.passed == 1u);
  teardown(&f);

  setup(&f);
  f.step = 101u;
  assert(h2_lingxi_e2e_run(&f.config, &result) == H2_PAL_ERR_TIMEOUT);
  assert(f.requests == 1u && result.failed == 1u && result.blocked == 1u &&
         result.passed == 1u);
  teardown(&f);
  return 0;
}
