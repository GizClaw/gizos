#include "h2_alipay_tiga_protocol.h"
#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_time.h"
#include "h2_app_test_sync.h"
#include "h2_alipay_test_pref.h"
#include "h2_alipay_tiga_storage.h"
#include "h2_alipay_e2e_independent.h"
#include "h2_alipay_context.h"
#include "h2_wolfcrypt_crypto.h"
#include "esp_mac.h"
#include "h2_alipay_api.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned requests, responses_freed;

static bool contains(const uint8_t* data, size_t length, const char* text) {
  size_t size = strlen(text);
  for (size_t index = 0u; index + size <= length; ++index)
    if (memcmp(data + index, text, size) == 0) return true;
  return false;
}

static int request(void* user, const h2_pal_http_request_t* request,
                   h2_pal_http_response_t* response) {
  (void)user;
  ++requests;
  static const char gateway[] = "https://iotapi.alipay.com/mgw.htm";
  assert(request->url.len == sizeof(gateway) - 1u);
  assert(memcmp(request->url.data, gateway, sizeof(gateway) - 1u) == 0);
  assert(contains(request->body, request->body_len,
                  requests == 1u   ? "ALIPAY_BRAND_NAME_H106"
                  : requests == 2u ? "\"deviceModel\":\"QZ.SB.LY.HAIVIVI-H106\""
                                   : "\"operationMode\":\"NETWORK\""));
  static const uint8_t name[] = {0x0a, 7u, 'D', 'E', 'V', 'I', 'C', 'E', '1'};
  static const uint8_t token[] = "{\"success\":true,\"token\":\"TEST_TOKEN\"}";
  static const uint8_t pending[] = "{\"needQuery\":true}";
  const uint8_t* data = requests == 1u   ? name
                        : requests == 2u ? token
                                         : pending;
  size_t length = requests == 1u ? sizeof(name) : strlen((const char*)data);
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
  static uint32_t state = 1u;
  for (size_t index = 0u; index < length; ++index) {
    state = state * 1664525u + 1013904223u;
    out[index] = (uint8_t)(state >> 24u);
  }
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
  };
  const h2_pal_crypto_api_t crypto = {NULL, &crypto_vtable};
  const h2_pal_pref_api_t* persistent = h2_alipay_test_pref_reset();
  h2_runtime_t runtime = {
      .mem = &mem.api,
      .time = &time.api,
      .sync = &sync.api,
      .http = &http,
      .crypto = &crypto,
      .pref = persistent,
  };
  h2_alipay_e2e_product_info_t product;
  h2_alipay_e2e_target_product_info(NULL);
  h2_alipay_e2e_target_product_info(&product);
  assert(product.company_rc != ALIPAY_RV_OK);
  assert(product.model_rc != ALIPAY_RV_OK);
  assert(product.company[0] == '\0' && product.model[0] == '\0');
  h2_pal_pref_namespace_t* old_namespace = NULL;
  assert(h2_pal_pref_open(persistent, "alipay_h106_ctx",
                          H2_PAL_PREF_OPEN_READ_WRITE,
                          &old_namespace) == H2_PAL_OK);
  static const char old_context[] = "OLD_MODEL_CONTEXT";
  assert(old_namespace->set_blob(old_namespace, "context", old_context,
                                 sizeof(old_context)) == H2_PAL_OK);
  assert(old_namespace->commit(old_namespace) == H2_PAL_OK);
  assert(old_namespace->close(old_namespace) == H2_PAL_OK);
  assert(h2_alipay_tiga_protocol_configure(&runtime, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  runtime.crypto = h2_wolfcrypt_crypto_api();
  assert(h2_alipay_tiga_protocol_configure(&runtime, 1794449000u) ==
         H2_PAL_ERR_UNSUPPORTED);
  runtime.crypto = &crypto;
  mem.fail_at = mem.calls + 1u;
  assert(h2_alipay_tiga_protocol_configure(&runtime, 1794449000u) ==
         H2_PAL_ERR_NO_MEMORY);
  mem.fail_at = 0u;
  uint8_t unavailable = 0u;
  assert(h2_pal_crypto_random(h2_wolfcrypt_crypto_api(), &unavailable, 1u) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(mem.live_blocks == 0u);
  sync.create = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  assert(h2_alipay_tiga_protocol_configure(&runtime, 1794449000u) ==
         H2_PAL_ERR_IO);
  assert(h2_pal_crypto_random(h2_wolfcrypt_crypto_api(), &unavailable, 1u) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(mem.live_blocks == 0u);
  assert(h2_alipay_tiga_protocol_configure(&runtime, 1794449000u) == H2_PAL_OK);
  const h2_pal_crypto_api_t* owned_crypto = h2_wolfcrypt_crypto_api();
  assert(h2_alipay_tiga_protocol_configure(&runtime, 1794449000u) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_alipay_e2e_target_product_info(&product);
  assert(product.company_rc == H2_PAL_OK && product.model_rc == H2_PAL_OK);
  assert(strcmp(product.company, "HAIVIVI") == 0);
  assert(strcmp(product.model, "H106") == 0);
  const h2_alipay_api_t* api = h2_alipay_e2e_target_api();
  assert(api != NULL && api->source == H2_ALIPAY_SOURCE_INDEPENDENT);
  assert(api->storage_scope == H2_ALIPAY_STORAGE_SESSION);
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
  assert(alipay_get_productType() == 3u);
  assert(!alipay_npay_get_quick_pay_flag());
  assert(alipay_npay_set_quick_pay_flag(true) == 0);
  assert(alipay_npay_get_quick_pay_flag());
  assert(alipay_npay_get_quick_pay_show_flag());
  assert(alipay_npay_set_quick_pay_flag(false) == 0);
  assert(!alipay_npay_get_quick_pay_flag());
  h2_alipay_binding_result_t binding;
  assert(api->vtable->binding_snapshot(api->user, &binding) == 0 &&
         binding.state == H2_ALIPAY_BINDING_UNBOUND);
  size_t baseline = mem.live_blocks;
  mem.fail_at = mem.calls + 1u;
  assert(api->vtable->get_binding_code(api->user, 16385u, code, sizeof(code),
                                       &length,
                                       &implementation_rc) == H2_PAL_ERR_IO);
  assert(implementation_rc != 0 && code[0] == '\0' && length == 0u);
  assert(mem.calls == mem.fail_at && mem.live_blocks == baseline);
  mem.fail_at = 0u;
  assert(h2_pal_time_set_wall_ms(&time.api, 1794449000000u) == H2_PAL_OK);
  int code_result = api->vtable->get_binding_code(
      api->user, 16385u, code, sizeof(code), &length, &implementation_rc);
  if (code_result != H2_PAL_OK) {
    h2_alipay_independent_binding_result_t diagnostic;
    assert(h2_alipay_independent_last_binding_result(&diagnostic) == 0);
    fprintf(
        stderr,
        "target code=%d sdk=%d core=%d stage=%d requests=%u http=%d rpc=%d\n",
        code_result, implementation_rc, diagnostic.result,
        (int)diagnostic.stage, diagnostic.requests, diagnostic.http_status,
        diagnostic.rpc_status);
  }
  assert(code_result == H2_PAL_OK);
  assert(implementation_rc == 0 && length > 0u && requests == 2u &&
         responses_freed == 2u);
  h2_alipay_independent_binding_result_t code_diagnostic;
  assert(h2_alipay_independent_last_binding_result(&code_diagnostic) == 0);
  assert(code_diagnostic.result == 0 &&
         code_diagnostic.stage == H2_ALIPAY_CLIENT_QR &&
         code_diagnostic.requests == 2u);
  assert(contains((const uint8_t*)code, length, "QZ.SB.LY.HAIVIVI-H106"));
  assert(h2_pal_pref_open(persistent, "h106_pay_ctx",
                          H2_PAL_PREF_OPEN_READ_ONLY,
                          &old_namespace) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_pref_open(persistent, "alipay_h106_ctx",
                          H2_PAL_PREF_OPEN_READ_ONLY,
                          &old_namespace) == H2_PAL_OK);
  void* old_bytes = NULL;
  size_t old_length = 0u;
  assert(old_namespace->get_blob(old_namespace, &mem.api, "context", &old_bytes,
                                 &old_length) == H2_PAL_ERR_NOT_FOUND);
  assert(old_length == 0u && old_bytes == NULL);
  h2_pal_mem_free(&mem.api, old_bytes);
  assert(old_namespace->close(old_namespace) == H2_PAL_OK);
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
  assert(api->vtable->query_binding_result(api->user, &binding) == 0);
  assert(binding.state == H2_ALIPAY_BINDING_PENDING && binding.sdk_rc == 0 &&
         binding.sdk_status == 0xff && requests == 3u && responses_freed == 3u);
  sync.destroy =
      (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u};
  assert(h2_alipay_tiga_protocol_release() == H2_PAL_ERR_IO);
  uint8_t retained = 0u;
  assert(h2_pal_crypto_random(owned_crypto, &retained, 1u) == 0);
  assert(h2_alipay_tiga_protocol_release() == H2_PAL_OK);
  assert(h2_alipay_tiga_protocol_release() == H2_PAL_OK);
  uint8_t entropy_byte = 0u;
  assert(h2_pal_crypto_random(owned_crypto, &entropy_byte, 1u) ==
         H2_PAL_ERR_UNAVAILABLE);
  assert(h2_pal_crypto_random(h2_wolfcrypt_crypto_api(), &entropy_byte, 1u) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_alipay_tiga_storage_audit() == 0);
  assert(h2_alipay_tiga_storage_release() == 0);
  assert(h2_alipay_test_pref_idle());
  assert(mem.live_blocks == 0u);
  const h2_wolfcrypt_crypto_config_t external_crypto = {NULL, random_bytes};
  assert(h2_wolfcrypt_crypto_init(&external_crypto) == 0);
  runtime.crypto = h2_wolfcrypt_crypto_api();
  assert(h2_alipay_tiga_protocol_configure(&runtime, 1794449000u) == 0);
  assert(h2_alipay_tiga_protocol_release() == 0);
  assert(h2_pal_crypto_random(runtime.crypto, &entropy_byte, 1u) == 0);
  assert(h2_alipay_tiga_storage_release() == 0);
  h2_wolfcrypt_crypto_deinit();
  assert(mem.live_blocks == 0u);
  return 0;
}
