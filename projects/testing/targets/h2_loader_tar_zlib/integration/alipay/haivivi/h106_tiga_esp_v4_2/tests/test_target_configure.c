#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_protocol.h"
#include "h2_alipay_e2e_independent.h"
#include "h2_alipay_api.h"
#include "h2_esp_board.h"

#include <assert.h>

static h2_runtime_t runtime;
static unsigned configured;
static int configure_result;
static const h2_alipay_payment_api_t payment = {0};

h2_pal_result_t h2_alipay_tiga_protocol_configure(h2_runtime_t* current,
                                                  uint32_t timestamp) {
  if (current == NULL) return H2_PAL_ERR_INVALID_ARG;
  (void)timestamp;
  assert(current == &runtime);
  ++configured;
  return configure_result;
}

const h2_alipay_payment_api_t* h2_alipay_e2e_independent_payment_api(void) {
  return &payment;
}

h2_display_info_t h2_esp_board_display_set_transfer_capture(
    h2_alipay_e2e_capture_fn capture, void* user) {
  (void)capture;
  (void)user;
  return (h2_display_info_t){0};
}

int main(void) {
  assert(h2_alipay_e2e_target_configure(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(configured == 0u);
  configure_result = H2_PAL_ERR_IO;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_ERR_IO);
  configure_result = H2_PAL_OK;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_OK);
  assert(configured == 2u && h2_alipay_e2e_target_complete_binding());
  assert(h2_alipay_e2e_target_feature() ==
         (FEATURE_PAYCODE | FEATURE_NFCPAY | FEATURE_SQUARE));
  assert(h2_alipay_e2e_target_payment_api() == &payment);
  return 0;
}
