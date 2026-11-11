#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_esp_alipay.h"
#include "h2_esp_board.h"

h2_pal_result_t h2_alipay_e2e_target_configure(h2_runtime_t* runtime) {
  const h2_esp_alipay_config_t config = {
      .mem = runtime->mem,
      .time = runtime->time,
      .log = runtime->log,
      .identity_user = runtime,
      .read_sn = h2_alipay_tiga_read_sn,
      .read_imei = h2_alipay_tiga_read_imei,
      .has_modem = true,
      .company = "HAIVIVI",
      .model = "H-106TG",
      .product_type = 3,
      .secure = NULL,
  };
  return h2_esp_alipay_configure(&config);
}

h2_display_info_t h2_alipay_e2e_target_capture(h2_alipay_e2e_capture_fn capture,
                                               void* user) {
  return h2_esp_board_display_set_transfer_capture(capture, user);
}
