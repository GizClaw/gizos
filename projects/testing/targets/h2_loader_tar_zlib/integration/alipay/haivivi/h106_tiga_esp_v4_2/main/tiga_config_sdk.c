#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_sdk.h"
#include "h2_esp_alipay.h"
#include "h2_esp_board.h"
#include "alipay_common.h"

uint32_t h2_alipay_e2e_target_feature(void) {
  return FEATURE_PAYCODE | FEATURE_SQUARE;
}

bool h2_alipay_e2e_target_complete_binding(void) { return true; }

h2_pal_result_t h2_alipay_e2e_target_configure(h2_runtime_t* runtime) {
  return h2_alipay_tiga_sdk_configure(runtime);
}

const h2_alipay_api_t* h2_alipay_e2e_target_api(void) {
  return h2_esp_alipay_api();
}

h2_display_info_t h2_alipay_e2e_target_capture(h2_alipay_e2e_capture_fn capture,
                                               void* user) {
  return h2_esp_board_display_set_transfer_capture(capture, user);
}
