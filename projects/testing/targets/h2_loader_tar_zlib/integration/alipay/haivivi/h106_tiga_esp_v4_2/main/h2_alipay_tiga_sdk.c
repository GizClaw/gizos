#include "h2_alipay_tiga_sdk.h"

#include "h2_alipay_tiga_identity.h"
#include "h2_esp_alipay.h"

h2_pal_result_t h2_alipay_tiga_sdk_configure(h2_runtime_t* runtime) {
  if (runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
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
      .company = "HAIVIVI",
      .model = "H-106TG",
      .product_type = 3,
      .pref = runtime->pref,
      .psa_namespace = "alipay_sn_psa",
      .certificate_namespace = "alipay_sn_cert",
  };
  return h2_esp_alipay_configure(&config);
}
