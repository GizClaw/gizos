#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_alipay_tiga_protocol.h"
#include "h2_esp_alipay.h"
#include "h2_esp_board.h"
#include "alipay_common.h"
#include <stdio.h>

uint32_t h2_alipay_e2e_target_feature(void) {
  return FEATURE_PAYCODE | FEATURE_SQUARE;
}

h2_pal_result_t h2_alipay_e2e_target_configure(h2_runtime_t* runtime) {
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
  int rc = h2_esp_alipay_configure(&config);
  printf("H2_ALIPAY_COMPARE phase=configure source=vendor_sdk rc=%d\n", rc);
  const h2_alipay_api_t* reference =
      rc == H2_PAL_OK ? h2_esp_alipay_api() : NULL;
  return h2_alipay_tiga_protocol_configure(runtime, reference);
}

h2_display_info_t h2_alipay_e2e_target_capture(h2_alipay_e2e_capture_fn capture,
                                               void* user) {
  return h2_esp_board_display_set_transfer_capture(capture, user);
}
