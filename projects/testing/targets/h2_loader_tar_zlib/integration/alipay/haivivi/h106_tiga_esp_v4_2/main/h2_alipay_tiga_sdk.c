#include "h2_alipay_tiga_sdk.h"

#include "h2_alipay_tiga_identity.h"
#include "h2_alipay_tiga_storage.h"
#include "h2_esp_alipay.h"
#include "h2_alipay_e2e_target.h"
#include "vendor_device_info.h"
#include <string.h>

h2_pal_result_t h2_alipay_tiga_sdk_configure(h2_runtime_t* runtime) {
  if (runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_pref_api_t* pref = NULL;
  int rc = h2_alipay_tiga_storage_configure(runtime, &pref);
  if (rc != H2_PAL_OK) return rc;
  const h2_esp_alipay_config_t config = {
      .mem = runtime->mem,
      .time = runtime->time,
      .log = runtime->log,
      .sync = runtime->sync,
      .crypto = runtime->crypto,
      .net = runtime->net,
      .netif = runtime->netif,
      .identity_user = runtime,
      .read_sn = h2_alipay_tiga_read_sn,
      .read_imei = h2_alipay_tiga_read_imei,
      .has_modem = true,
      .company = H2_ALIPAY_TIGA_COMPANY_NAME,
      .model = H2_ALIPAY_TIGA_PRODUCT_MODEL,
      .product_type = 3,
      .pref = pref,
      .storage_scope = h2_alipay_tiga_storage_scope(),
      .psa_namespace = "h106_pay_psa",
      .certificate_namespace = "h106_pay_cert",
  };
  return h2_esp_alipay_configure(&config);
}

void h2_alipay_e2e_target_product_info(h2_alipay_e2e_product_info_t* out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  uint32_t length = sizeof(out->company);
  out->company_rc = alipay_get_companyName((uint8_t*)out->company, &length);
  if (out->company_rc != ALIPAY_RV_OK) out->company[0] = '\0';
  out->company[sizeof(out->company) - 1u] = '\0';
  length = sizeof(out->model);
  out->model_rc = alipay_get_productModel((uint8_t*)out->model, &length);
  if (out->model_rc != ALIPAY_RV_OK) out->model[0] = '\0';
  out->model[sizeof(out->model) - 1u] = '\0';
}
