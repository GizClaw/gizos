#include "h2_alipay_context.h"

#include "h2_alipay_api.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_time.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int unexpected_request(void* user, const h2_pal_http_request_t* request,
                              h2_pal_http_response_t* response) {
  (void)user;
  (void)request;
  (void)response;
  assert(false);
  return H2_PAL_ERR_IO;
}

static void unchanged(const void* data, size_t length) {
  const uint8_t* bytes = data;
  for (size_t index = 0u; index < length; ++index)
    assert(bytes[index] == 0xa5u);
}

static void cache_script(const h2_alipay_client_config_t* config,
                         const char* mode) {
  char record[512];
  int length = snprintf(
      record, sizeof(record),
      "{\"TYPE\":{\"number\":\"NUMBER\",\"render\":{"
      "\"renderType\":\"%s\",\"expiredTime\":2000000000,"
      "\"offlineCert\":\"414243\",\"genCodeScript\":{\"scriptName\":\"TEST\"},"
      "\"config\":\"{\\\"QR_MG_TIM\\\":\\\"100\\\","
      "\\\"QR_OG_TIM\\\":\\\"20\\\"}\"}}}",
      mode);
  assert(length > 0 && (size_t)length < sizeof(record));
  h2_pal_pref_namespace_t* ns = NULL;
  assert(h2_pal_pref_open(config->pref, config->pref_namespace,
                          H2_PAL_PREF_OPEN_READ_WRITE, &ns) == 0);
  assert(ns->set_blob(ns, "transit_data", record, (size_t)length) == 0);
  assert(ns->commit(ns) == 0 && ns->close(ns) == 0);
}

int main(void) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &memory.api) == 0);
  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
  assert(h2_pal_time_set_wall_ms(&time.api, 1794449000000u) == 0);
  static const h2_pal_http_vtable_t http_vtable = {.request =
                                                       unexpected_request};
  const h2_pal_http_api_t http = {NULL, &http_vtable};
  static const h2_pal_crypto_vtable_t crypto_vtable = {0};
  const h2_pal_crypto_api_t crypto = {NULL, &crypto_vtable};
  const h2_alipay_client_config_t config = {
      .mem = &memory.api,
      .time = &time.api,
      .pref = &pref.api,
      .http = &http,
      .crypto = &crypto,
      .identity = {"TEST_SN", "TEST_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .pref_namespace = "payment-profile",
      .product_type = 3u,
      .feature = 0x4001u,
      .compile_timestamp_s = 1794449000u};
  assert(h2_alipay_independent_configure(&config, &sync.api) == 0);
  assert(alipay_pre_init() == 0);
  assert(alipay_init_transit_module() == 0);

  uint8_t image[64u * 64u];
  memset(image, 255, sizeof(image));
  alipay_union_code_order_info_t order;
  memset(&order, 0xa5, sizeof(order));
  size_t baseline = memory.live_blocks;
  assert(alipay_union_code_parse_with_img(image, 64u, 64u, &order) == 99);
  unchanged(&order, sizeof(order));
  h2_alipay_client_payment_t diagnostics;
  assert(h2_alipay_independent_last_payment_result(&diagnostics) == 0);
  assert(diagnostics.result == H2_PAL_ERR_UNSUPPORTED);
  assert(memory.live_blocks == baseline && pref.active_handles == 0u);
  assert(alipay_union_code_parse_with_img(NULL, 64u, 64u, &order) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(alipay_union_code_parse_with_img(image, 0u, 64u, &order) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(alipay_union_code_parse_with_img(image, UINT32_MAX, 64u, &order) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(alipay_union_code_parse_with_img(image, 64u, 64u, NULL) ==
         ALIPAY_RV_WRONG_PARAM);
  unchanged(&order, sizeof(order));

  static const char* const modes[] = {"SCRIPT", "ONLINE_PRIORITY"};
  for (size_t index = 0u; index < sizeof(modes) / sizeof(*modes); ++index) {
    cache_script(&config, modes[index]);
    baseline = memory.live_blocks;
    uint8_t output[64];
    char error[64];
    memset(output, 0xa5, sizeof(output));
    memset(error, 0xa5, sizeof(error));
    uint32_t length = sizeof(output);
    assert(alipay_transit_get_TransitCode("NUMBER", "TYPE", output, &length,
                                          error, sizeof(error)) == 43);
    assert(length == sizeof(output));
    unchanged(output, sizeof(output));
    assert(memory.live_blocks == baseline && pref.active_handles == 0u);
  }

  assert(h2_alipay_independent_release() == 0);
  assert(h2_app_test_pref_deinit(&pref) == 0);
  assert(memory.live_blocks == 0u);
  return 0;
}
