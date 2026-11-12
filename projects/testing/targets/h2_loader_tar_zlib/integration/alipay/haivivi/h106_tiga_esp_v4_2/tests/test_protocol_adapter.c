#include "h2_alipay_tiga_protocol.h"
#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_time.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_pref.h"
#include "h2_alipay_e2e_independent.h"
#include "esp_mac.h"

#include <assert.h>
#include <string.h>

static unsigned requests, responses_freed;

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  (void)user;
  ++requests;
  static const uint8_t name[] = {0x0a, 7u, 'D', 'E', 'V', 'I', 'C', 'E', '1'};
  static const uint8_t token[] = "{\"success\":true,\"token\":\"TEST_TOKEN\"}";
  const uint8_t* data = requests == 1u ? name : token;
  size_t length = requests == 1u ? sizeof(name) : sizeof(token) - 1u;
  assert(length <= request->response_buf_cap);
  memcpy(request->response_buf, data, length);
  response->status_code = 200;
  response->body = request->response_buf;
  response->body_len = length;
  return h2_pal_http_deliver_response_header(request, "Result-Status", 13u,
                                             "1000", 4u);
}

static void response_free(void* user, h2_pal_http_response_t* response) {
  (void)user;
  ++responses_freed;
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
  assert(key->bytes[0] == 0x42u);
  memset(out, 0x42, 65u);
  out[0] = 4u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_tiga_read_sn(void* user, char* out, size_t capacity) {
  assert(user != NULL && capacity >= sizeof("SYNTHETIC_FACTORY_SN"));
  memcpy(out, "SYNTHETIC_FACTORY_SN", sizeof("SYNTHETIC_FACTORY_SN"));
  return H2_PAL_OK;
}

int esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
  assert(type == ESP_MAC_BT);
  memcpy(mac, (uint8_t[]){0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}, 6u);
  return ESP_OK;
}

int main(void) {
  h2_app_test_mem_t mem;
  h2_app_test_mem_init(&mem, NULL);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  const h2_pal_http_vtable_t http_vtable = {.request = request,
                                            .response_free = response_free};
  const h2_pal_http_api_t http = {NULL, &http_vtable};
  const h2_pal_crypto_vtable_t crypto_vtable = {
      .random = random_bytes,
      .p256_keypair_generate = keypair,
      .p256_public_key_uncompress = uncompress,
  };
  const h2_pal_crypto_api_t crypto = {NULL, &crypto_vtable};
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &mem.api) == H2_PAL_OK);
  h2_runtime_t runtime = {
      .mem = &mem.api,
      .time = &time.api,
      .sync = &sync.api,
      .http = &http,
      .crypto = &crypto,
      .pref = &pref.api,
  };
  assert(h2_alipay_tiga_protocol_configure(&runtime, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_tiga_protocol_configure(&runtime, 1794449000u) == H2_PAL_OK);
  assert(h2_alipay_tiga_protocol_configure(&runtime, 1794449000u) ==
         H2_PAL_ERR_INVALID_STATE);
  const h2_alipay_api_t* api = h2_alipay_e2e_target_api();
  assert(api != NULL && api->source == H2_ALIPAY_SOURCE_INDEPENDENT);
  char code[H2_ALIPAY_BINDING_CODE_CAPACITY] = "previous code";
  size_t length = 13u;
  int implementation_rc = 1;
  assert(api->vtable->get_binding_code(api->user, 16385u, code, sizeof(code),
                                       &length, &implementation_rc) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(code[0] == '\0' && length == 0u &&
         implementation_rc == H2_PAL_ERR_INVALID_STATE);
  bool first = false;
  assert(api->vtable->pre_init(api->user, &implementation_rc, &first) ==
         H2_PAL_OK);
  assert(first && implementation_rc == H2_PAL_OK);
  assert(api->vtable->pre_init(api->user, &implementation_rc, &first) ==
         H2_PAL_OK);
  assert(!first && implementation_rc == H2_PAL_OK);
  size_t baseline = mem.live_blocks;
  mem.fail_at = mem.calls + 1u;
  assert(api->vtable->get_binding_code(api->user, 16385u, code, sizeof(code),
                                       &length,
                                       &implementation_rc) == H2_PAL_ERR_IO);
  assert(implementation_rc != 0 && code[0] == '\0' && length == 0u);
  assert(mem.calls == mem.fail_at && mem.live_blocks == baseline);
  mem.fail_at = 0u;
  assert(h2_pal_time_set_wall_ms(&time.api, 1794449000000u) == H2_PAL_OK);
  assert(api->vtable->get_binding_code(api->user, 16385u, code, sizeof(code),
                                       &length,
                                       &implementation_rc) == H2_PAL_OK);
  assert(implementation_rc == 0 && length > 0u && requests == 2u &&
         responses_freed == 2u);
  unsigned observed = 0u;
  for (size_t index = 0u; index < api->trace->count; ++index) {
    const h2_alipay_trace_event_t* event = &api->trace->events[index];
    assert(event->length == 0u && event->captured == 0u);
    if (strcmp(event->operation, "independent.http.return") == 0) {
      assert(event->result == H2_PAL_OK && event->item == 200u);
      ++observed;
    }
  }
  assert(observed == 2u);
  assert(h2_alipay_e2e_independent_release() == H2_PAL_OK);
  assert(h2_app_test_pref_deinit(&pref) == H2_PAL_OK);
  assert(mem.live_blocks == 0u);
  return 0;
}
