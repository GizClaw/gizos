#include "h2_alipay_tiga_protocol.h"

#include "h2_alipay_e2e_independent.h"
#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_alipay_tiga_storage.h"
#include "h2_wolfcrypt_crypto.h"
#include "esp_mac.h"
#include "h2_alipay_api.h"
#include <stdio.h>
#include <string.h>

static h2_runtime_t* s_runtime;
static h2_alipay_client_config_t s_config;
static char s_sn[49], s_mac[18];
static bool s_crypto_owned;

static int crypto_entropy(void* user, uint8_t* output, size_t length) {
  return h2_pal_crypto_random(user, output, length);
}

static int configure_crypto(const h2_runtime_t* runtime,
                            const h2_pal_crypto_api_t** output) {
  *output = NULL;
  if (runtime->crypto == NULL) return H2_PAL_ERR_INVALID_ARG;
  uint8_t probe = 0u;
  int rc = h2_pal_crypto_random(runtime->crypto, &probe, 1u);
  if (rc != H2_PAL_OK) return rc;
  const h2_pal_crypto_api_t* current = h2_wolfcrypt_crypto_api();
  if (runtime->crypto == current) {
    if (current->vtable == NULL ||
        current->vtable->ecdsa_secp192k1_sha1_sign == NULL ||
        current->vtable->sm2_sign_digest == NULL ||
        current->vtable->des_cbc == NULL)
      return H2_PAL_ERR_UNSUPPORTED;
    *output = current;
    return H2_PAL_OK;
  }
  const h2_wolfcrypt_crypto_config_t config = {
      .entropy_user = (void*)runtime->crypto,
      .entropy = crypto_entropy,
  };
  rc = h2_wolfcrypt_crypto_init(&config);
  if (rc == H2_PAL_OK) {
    s_crypto_owned = true;
    *output = h2_wolfcrypt_crypto_api();
  }
  return rc;
}

static void release_crypto(void) {
  if (s_crypto_owned) {
    h2_wolfcrypt_crypto_deinit();
    s_crypto_owned = false;
  }
}

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
  const h2_pal_crypto_api_t* crypto = NULL;
  rc = configure_crypto(runtime, &crypto);
  if (rc != H2_PAL_OK) {
    (void)h2_alipay_tiga_storage_release();
    return rc;
  }
  s_config = (h2_alipay_client_config_t){
      .mem = runtime->mem,
      .time = runtime->time,
      .http = runtime->http,
      .gateway_url = "https://iotapi.alipay.com/mgw.htm",
      .pref = pref,
      .crypto = crypto,
      .identity = {.sn = s_sn,
                   .imei = s_sn,
                   .mac = s_mac,
                   .model = H2_ALIPAY_TIGA_PRODUCT_MODEL},
      .company = H2_ALIPAY_TIGA_COMPANY_NAME,
      .product_type = 3u,
      .pref_namespace = "h106_pay_ctx",
      .feature = H2_ALIPAY_TIGA_FEATURE_MASK,
      .compile_timestamp_s = compile_timestamp_s,
  };
  rc = h2_alipay_e2e_independent_configure_with_storage(
      &s_config, runtime->sync, h2_alipay_tiga_storage_scope());
  if (rc != H2_PAL_OK) {
    release_crypto();
    (void)h2_alipay_tiga_storage_release();
    memset(&s_config, 0, sizeof(s_config));
    return rc;
  }
  s_runtime = runtime;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_tiga_protocol_release(void) {
  int rc = h2_alipay_e2e_independent_release();
  if (rc != H2_PAL_OK) return rc;
  release_crypto();
  s_runtime = NULL;
  memset(&s_config, 0, sizeof(s_config));
  memset(s_sn, 0, sizeof(s_sn));
  memset(s_mac, 0, sizeof(s_mac));
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
