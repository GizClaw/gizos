#include "h2_alipay_client.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_time.h"
#include "core_http_client.h"

#include <assert.h>
#include <string.h>

typedef struct fixture {
  h2_app_test_mem_t mem;
  h2_app_test_pref_t pref;
  h2_app_test_time_t time;
  h2_pal_http_api_t http;
  h2_pal_crypto_api_t crypto;
  h2_alipay_client_config_t config;
  unsigned registrations, preparations, frees;
  int rpc_status, transport_result, crypto_result;
  const char* response;
} fixture_t;

static bool equal(h2_pal_http_str_t span, const char* text) {
  return span.len == strlen(text) && memcmp(span.data, text, span.len) == 0;
}

static h2_pal_http_str_t header(const h2_pal_http_request_t* request,
                                const char* name) {
  for (size_t index = 0u; index < request->header_count; ++index)
    if (equal(request->headers[index].name, name))
      return request->headers[index].value;
  return (h2_pal_http_str_t){0};
}

static int http_request(void* user, const h2_pal_http_request_t* request,
                        h2_pal_http_response_t* response) {
  fixture_t* fixture = user;
  assert(request->method == H2_PAL_HTTP_POST);
  assert(equal(request->url, "https://iotapi.alipay.com/mgw.htm"));
  assert(request->retry_count == 0 && request->timeout_ms > 0 &&
         request->connect_timeout_ms > 0);
  assert(equal(header(request, "AppId"), "AlipayInside_IoTPay_NET"));
  assert(equal(header(request, "Ts"), "1794449000000"));
  uint8_t header_buffer[4096];
  HTTPRequestHeaders_t encoded = {.pBuffer = header_buffer,
                                  .bufferLen = sizeof(header_buffer)};
  const HTTPRequestInfo_t info = {
      .pMethod = "POST",
      .methodLen = sizeof("POST") - 1u,
      .pHost = "iotapi.alipay.com",
      .hostLen = sizeof("iotapi.alipay.com") - 1u,
      .pPath = "/mgw.htm",
      .pathLen = sizeof("/mgw.htm") - 1u,
  };
  assert(HTTPClient_InitializeRequestHeaders(&encoded, &info) == HTTPSuccess);
  for (size_t index = 0u; index < request->header_count; ++index) {
    const h2_pal_http_header_t* item = &request->headers[index];
    assert(HTTPClient_AddHeader(&encoded, item->name.data, item->name.len,
                                item->value.data,
                                item->value.len) == HTTPSuccess);
  }
  bool registration =
      equal(header(request, "Operation-Type"), "iothub.internal.device.active");
  if (registration) {
    ++fixture->registrations;
    assert(equal(header(request, "Content-Type"), "application/protobuf"));
    assert(header(request, "Sign").len == 0u);
    assert(header(request, "did").len == 0u);
    assert(request->body_len > 0u && request->body[0] == 0x0au);
  } else {
    ++fixture->preparations;
    assert(equal(header(request, "Operation-Type"),
                 "alipay.mobile.alideviceinfo.changecode.prepareBind"));
    assert(equal(header(request, "Content-Type"), "application/json"));
    assert(header(request, "Sign").len == 64u);
    assert(equal(header(request, "signType"), "0"));
    assert(equal(header(request, "did"), "SYNTHETIC_DEVICE_NAME_001"));
    assert(request->body_len > 0u && request->body[0] == '[');
  }
  if (fixture->transport_result != 0) return fixture->transport_result;
  const char* status = fixture->rpc_status == 7002 ? "7002" : "1000";
  int rc = h2_pal_http_deliver_response_header(request, "Result-Status", 13u,
                                               status, 4u);
  if (rc != H2_PAL_OK) return rc;
  static const uint8_t name[] = {
      0x0a, 0x19, 'S', 'Y', 'N', 'T', 'H', 'E', 'T', 'I', 'C', '_', 'D', 'E',
      'V',  'I',  'C', 'E', '_', 'N', 'A', 'M', 'E', '_', '0', '0', '1',
  };
  static const char success[] =
      "{\"success\":true,\"token\":\"SYNTHETIC_BINDING_TOKEN\"}";
  const uint8_t* data =
      registration
          ? name
          : (const uint8_t*)(fixture->response != NULL ? fixture->response
                                                       : success);
  size_t length = registration ? sizeof(name) : strlen((const char*)data);
  assert(length <= request->response_buf_cap);
  memcpy(request->response_buf, data, length);
  response->status_code = 200;
  response->body = request->response_buf;
  response->body_len = length;
  return H2_PAL_OK;
}

static void response_free(void* user, h2_pal_http_response_t* response) {
  fixture_t* fixture = user;
  ++fixture->frees;
  h2_pal_http_response_reset(response);
}

static h2_pal_result_t random_bytes(void* user, uint8_t* out, size_t length) {
  fixture_t* fixture = user;
  if (fixture->crypto_result != 0) return fixture->crypto_result;
  memset(out, 0x42, length);
  return H2_PAL_OK;
}

static h2_pal_result_t keypair_generate(void* user,
                                        h2_pal_p256_keypair_t* out) {
  fixture_t* fixture = user;
  if (fixture->crypto_result != 0) return fixture->crypto_result;
  memset(out, 0x42, sizeof(*out));
  return H2_PAL_OK;
}

static h2_pal_result_t uncompress(void* user,
                                  const h2_pal_p256_public_key_t* public_key,
                                  uint8_t out[65]) {
  (void)user;
  assert(public_key->bytes[0] == 0x42u);
  out[0] = 0x04u;
  for (size_t index = 0u; index < 64u; ++index)
    out[index + 1u] = (uint8_t)index;
  return H2_PAL_OK;
}

static void initialize(fixture_t* fixture) {
  memset(fixture, 0, sizeof(*fixture));
  h2_app_test_mem_init(&fixture->mem, NULL);
  assert(h2_app_test_pref_init(&fixture->pref, &fixture->mem.api) == H2_PAL_OK);
  h2_app_test_time_init(&fixture->time, 0u);
  assert(h2_pal_time_set_wall_ms(&fixture->time.api, 1794449000000u) ==
         H2_PAL_OK);
  static const h2_pal_http_vtable_t http = {.request = http_request,
                                            .response_free = response_free};
  static const h2_pal_crypto_vtable_t crypto = {
      .random = random_bytes,
      .p256_keypair_generate = keypair_generate,
      .p256_public_key_uncompress = uncompress,
  };
  fixture->http = (h2_pal_http_api_t){fixture, &http};
  fixture->crypto = (h2_pal_crypto_api_t){fixture, &crypto};
  fixture->config = (h2_alipay_client_config_t){
      .mem = &fixture->mem.api,
      .pref = &fixture->pref.api,
      .time = &fixture->time.api,
      .crypto = &fixture->crypto,
      .http = &fixture->http,
      .identity =
          {
              .sn = "FACTORY_SN_001",
              .imei = "FACTORY_SN_001",
              .mac = "12:34:56:78:9A:BC",
              .model = "H-106TG",
          },
      .company = "HAIVIVI",
      .pref_namespace = "alipay_direct",
      .feature = 16385u,
      .compile_timestamp_s = 1794449000u,
  };
}

static void destroy(fixture_t* fixture) {
  assert(fixture->pref.active_handles == 0u);
  assert(h2_app_test_pref_deinit(&fixture->pref) == H2_PAL_OK);
  assert(fixture->mem.live_blocks == 0u);
}

static void failure_clears(const h2_alipay_client_result_t* result) {
  assert(result->code[0] == '\0' && result->length == 0u &&
         result->token[0] == '\0');
  assert(memcmp(result->private_key.bytes, (uint8_t[32]){0}, 32u) == 0);
}

int main(void) {
  fixture_t fixture;
  h2_alipay_client_result_t result;
  initialize(&fixture);
  size_t baseline_allocations = fixture.mem.calls;
  assert(h2_alipay_client_get_binding_code(&fixture.config, &result) ==
         H2_PAL_OK);
  size_t allocation_count = fixture.mem.calls - baseline_allocations;
  assert(result.registered && result.requests == 2u &&
         result.rpc_status == 1000);
  assert(result.length == strlen(result.code));
  assert(strcmp(result.code,
                "ALIPAY_QZWT#0#SYNTHETIC_BINDING_TOKEN#"
                "QZ.SB.LY.HAIVIVI-H-106TG#4.612.16385.179444") == 0);
  assert(result.private_key.bytes[0] == 0x42u);
  assert(h2_alipay_client_get_binding_code(&fixture.config, &result) ==
         H2_PAL_OK);
  assert(!result.registered && result.requests == 1u);
  assert(fixture.registrations == 1u && fixture.preparations == 2u &&
         fixture.frees == 3u);
  unsigned requests = fixture.registrations + fixture.preparations;
  fixture.config.identity.imei = "DIFFERENT_IDENTITY";
  assert(h2_alipay_client_get_binding_code(&fixture.config, &result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.registrations + fixture.preparations == requests);
  failure_clears(&result);
  destroy(&fixture);

  initialize(&fixture);
  fixture.config.identity.model = "MODEL#WITH_DELIMITER";
  assert(h2_alipay_client_get_binding_code(&fixture.config, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(fixture.registrations == 0u && fixture.preparations == 0u);
  failure_clears(&result);
  destroy(&fixture);

  initialize(&fixture);
  fixture.rpc_status = 7002;
  assert(h2_alipay_client_get_binding_code(&fixture.config, &result) ==
         H2_PAL_ERR_IO);
  assert(result.http_status == 200 && result.rpc_status == 7002 &&
         fixture.preparations == 0u);
  failure_clears(&result);
  destroy(&fixture);

  initialize(&fixture);
  fixture.pref.commit =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_WRITE, .remaining = 1u};
  assert(h2_alipay_client_get_binding_code(&fixture.config, &result) ==
         H2_PAL_ERR_WRITE);
  assert(result.registered && fixture.preparations == 0u);
  failure_clears(&result);
  destroy(&fixture);

  initialize(&fixture);
  fixture.transport_result = H2_PAL_ERR_TIMEOUT;
  assert(h2_alipay_client_get_binding_code(&fixture.config, &result) ==
         H2_PAL_ERR_TIMEOUT);
  assert(result.requests == 1u && fixture.frees == 1u);
  failure_clears(&result);
  destroy(&fixture);

  static const char* malformed[] = {
      "{\"success\":false,\"token\":\"x\"}",
      "{\"success\":true,\"token\":\"x\",\"token\":\"y\"}",
      "{\"success\":true,\"success\":true,\"token\":\"x\"}",
      "{\"success\":true,\"token\":1}",
      "{\"success\":true,\"token\":\"x\\u0000y\"}",
      "{\"success\":true,\"token\":\"x#y\"}",
      "{\"success\":true,\"token\":\"x\"}trailing",
  };
  for (size_t index = 0u; index < sizeof(malformed) / sizeof(*malformed);
       ++index) {
    initialize(&fixture);
    fixture.response = malformed[index];
    assert(h2_alipay_client_get_binding_code(&fixture.config, &result) !=
           H2_PAL_OK);
    failure_clears(&result);
    destroy(&fixture);
  }
  bool failed_before_network = false, failed_after_registration = false;
  bool failed_after_prepare = false;
  for (size_t index = 1u; index <= allocation_count; ++index) {
    initialize(&fixture);
    fixture.mem.fail_at = fixture.mem.calls + index;
    int rc = h2_alipay_client_get_binding_code(&fixture.config, &result);
    if (rc != H2_PAL_OK) {
      failed_before_network |= result.requests == 0u;
      failed_after_registration |= result.requests == 1u;
      failed_after_prepare |= result.requests == 2u;
      failure_clears(&result);
    }
    destroy(&fixture);
  }
  assert(failed_before_network && failed_after_registration &&
         failed_after_prepare);
  h2_alipay_client_result_clear(&result);
  const h2_alipay_client_result_t empty = {0};
  assert(memcmp(&result, &empty, sizeof(result)) == 0);
  return 0;
}
