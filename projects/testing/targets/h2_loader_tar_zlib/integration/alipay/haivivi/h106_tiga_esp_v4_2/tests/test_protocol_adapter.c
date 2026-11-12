#include "h2_alipay_tiga_protocol.h"
#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_time.h"
#include "esp_mac.h"

#include <assert.h>
#include <string.h>

static unsigned reference_initializations;

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

static h2_pal_result_t reference_pre_init(void* user, int* sdk_rc,
                                          bool* first) {
  (void)user;
  ++reference_initializations;
  *sdk_rc = -71;
  *first = true;
  return H2_PAL_ERR_IO;
}

int main(void) {
  h2_app_test_mem_t mem;
  h2_app_test_mem_init(&mem, NULL);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
  const h2_pal_http_api_t http = {0};
  const h2_pal_crypto_api_t crypto = {0};
  const h2_pal_pref_api_t pref = {0};
  h2_runtime_t runtime = {
      .mem = &mem.api,
      .time = &time.api,
      .http = &http,
      .crypto = &crypto,
      .pref = &pref,
  };
  static h2_alipay_trace_t trace;
  h2_alipay_trace_reset(&trace, false);
  const h2_alipay_vtable_t vtable = {.pre_init = reference_pre_init};
  const h2_alipay_api_t reference = {.vtable = &vtable, .trace = &trace};
  assert(h2_alipay_tiga_protocol_configure(&runtime, &reference) == H2_PAL_OK);
  assert(h2_alipay_tiga_protocol_configure(&runtime, &reference) ==
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
  assert(reference_initializations == 1u);
  mem.fail_at = mem.calls + 1u;
  assert(api->vtable->get_binding_code(api->user, 16385u, code, sizeof(code),
                                       &length, &implementation_rc) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(implementation_rc == H2_PAL_ERR_NO_MEMORY && code[0] == '\0' &&
         length == 0u);
  assert(mem.calls == mem.fail_at && mem.live_blocks == 0u);
  return 0;
}
