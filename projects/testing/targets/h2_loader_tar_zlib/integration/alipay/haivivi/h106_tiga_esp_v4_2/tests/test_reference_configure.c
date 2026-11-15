#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_alipay_tiga_storage.h"
#include "h2_memory_pref.h"
#include "h2_app_test_mem.h"
#include "h2_esp_alipay.h"
#include "h2_esp_board.h"
#include "vendor_device_info.h"
#include <string.h>

#include <assert.h>

static h2_runtime_t runtime;
static h2_alipay_api_t reference;
static unsigned sdk_calls;
static int sdk_result;
static int company_result;
static unsigned product_getters;
static h2_esp_alipay_config_t configured;

h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config) {
  assert(config->identity_user == &runtime);
  assert(strcmp(config->company, "HAIVIVI") == 0);
  assert(strcmp(config->model, "H106") == 0);
  assert(config->product_type == 3u);
  assert(config->storage_scope == H2_ALIPAY_STORAGE_SESSION);
  assert(config->pref != runtime.pref);
  assert(strcmp(config->psa_namespace, "h106_pay_psa") == 0);
  assert(strcmp(config->certificate_namespace, "h106_pay_cert") == 0);
  configured = *config;
  ++sdk_calls;
  return sdk_result;
}

retval_e alipay_get_companyName(uint8_t* out, uint32_t* length) {
  ++product_getters;
  if (company_result != 0) return (retval_e)company_result;
  assert(*length > strlen(configured.company));
  memcpy(out, configured.company, strlen(configured.company) + 1u);
  *length = (uint32_t)strlen(configured.company);
  return ALIPAY_RV_OK;
}

retval_e alipay_get_productModel(uint8_t* out, uint32_t* length) {
  ++product_getters;
  assert(*length > strlen(configured.model));
  memcpy(out, configured.model, strlen(configured.model) + 1u);
  *length = (uint32_t)strlen(configured.model);
  return ALIPAY_RV_OK;
}

const h2_alipay_api_t* h2_esp_alipay_api(void) {
  assert(sdk_result == H2_PAL_OK);
  return &reference;
}

h2_pal_result_t h2_alipay_tiga_read_sn(void* user, char* out, size_t capacity) {
  (void)user;
  assert(capacity >= sizeof("TEST_SN"));
  memcpy(out, "TEST_SN", sizeof("TEST_SN"));
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_tiga_read_imei(void* user, char* out,
                                         size_t capacity) {
  return h2_alipay_tiga_read_sn(user, out, capacity);
}

h2_display_info_t h2_esp_board_display_set_transfer_capture(
    h2_alipay_e2e_capture_fn capture, void* user) {
  (void)capture;
  (void)user;
  return (h2_display_info_t){0};
}

int main(void) {
  h2_app_test_mem_t mem;
  h2_app_test_mem_init(&mem, NULL);
  const h2_memory_pref_config_t pref_config = {&mem.api, 8u, 16u, 4096u, 8u};
  h2_memory_pref_t* persistent = NULL;
  assert(h2_memory_pref_create(&pref_config, &persistent) == 0);
  runtime.mem = &mem.api;
  runtime.pref = h2_memory_pref_api(persistent);
  assert(h2_alipay_e2e_target_configure(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(sdk_calls == 0u);
  sdk_result = H2_PAL_ERR_IO;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_ERR_IO);
  assert(sdk_calls == 1u);
  sdk_result = H2_PAL_OK;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_OK);
  assert(sdk_calls == 2u && h2_alipay_e2e_target_api() == &reference);
  assert(!h2_alipay_e2e_target_complete_binding());
  h2_alipay_e2e_target_product_info(NULL);
  assert(product_getters == 0u);
  h2_alipay_e2e_product_info_t product;
  h2_alipay_e2e_target_product_info(&product);
  assert(product_getters == 2u);
  assert(product.company_rc == ALIPAY_RV_OK &&
         product.model_rc == ALIPAY_RV_OK);
  assert(strcmp(product.company, "HAIVIVI") == 0);
  assert(strcmp(product.model, "H106") == 0);
  company_result = ALIPAY_RV_HAL_ERROR;
  h2_alipay_e2e_target_product_info(&product);
  assert(product_getters == 4u && sdk_calls == 2u);
  assert(product.company_rc == ALIPAY_RV_HAL_ERROR &&
         product.company[0] == '\0');
  assert(product.model_rc == ALIPAY_RV_OK &&
         strcmp(product.model, "H106") == 0);
  assert(h2_alipay_tiga_storage_release() == 0);
  assert(h2_memory_pref_destroy(&persistent) == 0);
  assert(mem.live_blocks == 0u);
  return 0;
}
