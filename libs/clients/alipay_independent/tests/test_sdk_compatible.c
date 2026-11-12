#include "h2_alipay_independent.h"
#include "alipay_account_manage.h"
#include "alipay_bind.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"

#include <assert.h>
#include <string.h>

typedef struct fixture {
  unsigned registrations, preparations;
  int network_rc;
} fixture_t;

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  fixture_t* fixture = user;
  bool registration = false;
  for (size_t index = 0u; index < request->header_count; ++index) {
    const h2_pal_http_header_t* header = &request->headers[index];
    if (header->name.len == sizeof("Operation-Type") - 1u &&
        memcmp(header->name.data, "Operation-Type", header->name.len) == 0)
      registration =
          header->value.len == sizeof("iothub.internal.device.active") - 1u &&
          memcmp(header->value.data, "iothub.internal.device.active",
                 header->value.len) == 0;
  }
  if (registration)
    ++fixture->registrations;
  else
    ++fixture->preparations;
  if (fixture->network_rc != H2_PAL_OK) return fixture->network_rc;
  static const uint8_t device_name[] = {0x0a, 7u,  'D', 'E', 'V',
                                        'I',  'C', 'E', '1'};
  static const uint8_t token[] = "{\"success\":true,\"token\":\"TEST_TOKEN\"}";
  const uint8_t* bytes = registration ? device_name : token;
  size_t length = registration ? sizeof(device_name) : sizeof(token) - 1u;
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
  (void)user;
  memset(out, 0x42, length);
  return H2_PAL_OK;
}

static int keypair(void* user, h2_pal_p256_keypair_t* out) {
  (void)user;
  memset(out, 0x42, sizeof(*out));
  return H2_PAL_OK;
}

static int uncompress(void* user, const h2_pal_p256_public_key_t* key,
                      uint8_t out[65]) {
  (void)user;
  assert(key->bytes[0] == 0x42);
  memset(out, 0x42, 65u);
  out[0] = 4u;
  return H2_PAL_OK;
}

int main(void) {
  assert(alipay_pre_init() != ALIPAY_RV_OK);
  assert(!alipay_get_binding_status());
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
  const h2_pal_crypto_vtable_t crypto_vtable = {
      .random = random_bytes,
      .p256_keypair_generate = keypair,
      .p256_public_key_uncompress = uncompress,
  };
  const h2_pal_crypto_api_t crypto = {NULL, &crypto_vtable};
  const h2_alipay_client_config_t config = {
      .mem = &mem.api,
      .pref = &pref.api,
      .time = &time.api,
      .crypto = &crypto,
      .http = &http,
      .identity = {.sn = "TEST_SN_001",
                   .imei = "TEST_SN_001",
                   .mac = "12:34:56:78:9A:BC",
                   .model = "MODEL_001"},
      .company = "TEST",
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
  assert(alipay_query_binding_result(&status) != ALIPAY_RV_OK);
  assert(status == ALIPAY_STATUS_BINDING_FAIL);
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
  assert(h2_alipay_independent_release() == H2_PAL_OK);
  assert(h2_app_test_pref_deinit(&pref) == H2_PAL_OK);
  assert(mem.live_blocks == 0u);
  return 0;
}
