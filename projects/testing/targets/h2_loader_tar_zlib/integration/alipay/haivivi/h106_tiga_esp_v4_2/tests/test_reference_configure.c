#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_esp_alipay.h"
#include "h2_esp_board.h"

#include <assert.h>

static h2_runtime_t runtime;
static h2_alipay_api_t reference;
static unsigned sdk_calls;
static int sdk_result;

h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config) {
  assert(config->identity_user == &runtime);
  ++sdk_calls;
  return sdk_result;
}

const h2_alipay_api_t* h2_esp_alipay_api(void) {
  assert(sdk_result == H2_PAL_OK);
  return &reference;
}

h2_pal_result_t h2_alipay_tiga_read_sn(void* user, char* out, size_t capacity) {
  (void)user;
  (void)out;
  (void)capacity;
  return H2_PAL_ERR_UNSUPPORTED;
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
  assert(h2_alipay_e2e_target_configure(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(sdk_calls == 0u);
  sdk_result = H2_PAL_ERR_IO;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_ERR_IO);
  assert(sdk_calls == 1u);
  sdk_result = H2_PAL_OK;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_OK);
  assert(sdk_calls == 2u && h2_alipay_e2e_target_api() == &reference);
  assert(!h2_alipay_e2e_target_complete_binding());
  return 0;
}
