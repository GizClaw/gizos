#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_protocol.h"
#include "h2_esp_board.h"

uint32_t h2_alipay_e2e_target_feature(void) { return 16385u; }

h2_pal_result_t h2_alipay_e2e_target_configure(h2_runtime_t* runtime) {
  return h2_alipay_tiga_protocol_configure(runtime, NULL);
}

h2_display_info_t h2_alipay_e2e_target_capture(h2_alipay_e2e_capture_fn capture,
                                               void* user) {
  return h2_esp_board_display_set_transfer_capture(capture, user);
}
