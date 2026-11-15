#include "h2_alipay_tiga_protocol.h"

#include "h2_alipay_e2e_independent.h"
#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_alipay_tiga_storage.h"
#include "esp_mac.h"
#include "vendor_device_info.h"
#include <stdio.h>
#include <string.h>

static h2_runtime_t* s_runtime;
static h2_alipay_client_config_t s_config;
static char s_sn[49], s_mac[18];

h2_pal_result_t h2_alipay_tiga_protocol_configure(
    h2_runtime_t* runtime, uint32_t compile_timestamp_s) {
  if (runtime == NULL || compile_timestamp_s == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_runtime != NULL) return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_alipay_tiga_read_sn(runtime, s_sn, sizeof(s_sn));
  uint8_t mac[6];
  if (rc == H2_PAL_OK)
    rc = esp_read_mac(mac, ESP_MAC_BT) == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
  if (rc != H2_PAL_OK) return rc;
  (void)snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
                 (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
  const h2_pal_pref_api_t* pref = NULL;
  rc = h2_alipay_tiga_storage_configure(runtime, &pref);
  if (rc != H2_PAL_OK) return rc;
  s_config = (h2_alipay_client_config_t){
      .mem = runtime->mem,
      .time = runtime->time,
      .http = runtime->http,
      .pref = pref,
      .crypto = runtime->crypto,
      .identity = {.sn = s_sn,
                   .imei = s_sn,
                   .mac = s_mac,
                   .model = H2_ALIPAY_TIGA_PRODUCT_MODEL},
      .company = H2_ALIPAY_TIGA_COMPANY_NAME,
      .product_type = 3u,
      .pref_namespace = "h106_pay_ctx",
      .feature = 16385u,
      .compile_timestamp_s = compile_timestamp_s,
  };
  rc = h2_alipay_e2e_independent_configure_with_storage(
      &s_config, runtime->sync, h2_alipay_tiga_storage_scope());
  if (rc != H2_PAL_OK) return rc;
  s_runtime = runtime;
  return H2_PAL_OK;
}

const h2_alipay_api_t* h2_alipay_e2e_target_api(void) {
  return h2_alipay_e2e_independent_api();
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
