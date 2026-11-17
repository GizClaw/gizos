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

static void unchanged(const alipay_union_code_order_info_t* order) {
  const uint8_t* bytes = (const uint8_t*)order;
  for (size_t index = 0u; index < sizeof(*order); ++index)
    assert(bytes[index] == 0xa5u);
}

int main(void) {
  uint8_t image[64u * 64u * 3u / 2u];
  memset(image, 255, sizeof(image));
  alipay_union_code_order_info_t order;
  memset(&order, 0xa5, sizeof(order));
  assert(alipay_union_code_parse_with_img(NULL, 64u, 64u, &order) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(alipay_union_code_parse_with_img(image, 0u, 64u, &order) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(alipay_union_code_parse_with_img(image, UINT32_MAX, 64u, &order) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(alipay_union_code_parse_with_img(image, 64u, 64u, NULL) ==
         ALIPAY_RV_WRONG_PARAM);
  unchanged(&order);
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_app_test_pref_t pref;
  assert(h2_app_test_pref_init(&pref, &memory.api) == 0);
  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0u);
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
      .identity = {"SYNTHETIC_SN", "SYNTHETIC_SN", "12:34:56:78:9A:BC", "H106"},
      .company = "HAIVIVI",
      .pref_namespace = "image",
      .product_type = 3u,
      .feature = 16385u,
      .compile_timestamp_s = 1794449000u};
  assert(h2_alipay_independent_configure(&config, &sync.api) == 0);
  assert(alipay_pre_init() == 0);
  size_t baseline = memory.live_blocks, start = memory.calls;
  assert(alipay_union_code_parse_with_img(image, 64u, 64u, &order) == 99);
  size_t allocations = memory.calls - start;
  assert(memory.live_blocks == baseline && pref.active_handles == 0u);
  unchanged(&order);
  for (size_t fail = 1u; fail <= allocations; ++fail) {
    memory.fail_at = memory.calls + fail;
    assert(alipay_union_code_parse_with_img(image, 64u, 64u, &order) == 29);
    memory.fail_at = 0u;
    unchanged(&order);
    assert(memory.live_blocks == baseline && pref.active_handles == 0u);
    h2_alipay_client_payment_t diagnostics;
    assert(h2_alipay_independent_last_payment_result(&diagnostics) == 0);
    assert(diagnostics.result == H2_PAL_ERR_NO_MEMORY &&
           diagnostics.state == H2_ALIPAY_CLIENT_PAYMENT_UNKNOWN);
  }
  assert(h2_alipay_independent_release() == 0);
  assert(h2_app_test_pref_deinit(&pref) == 0);
  assert(memory.live_blocks == 0u);
  printf("NV12 facade allocation failure points=%zu\n", allocations);
  return 0;
}
