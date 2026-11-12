#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "h2_alipay_tiga_protocol.h"
#include "h2_esp_alipay.h"
#include "h2_esp_board.h"

#include <assert.h>

static h2_runtime_t runtime;
static h2_alipay_api_t reference;
static const h2_alipay_api_t* observed_reference;
static unsigned sdk_calls, independent_calls;
static int sdk_result, independent_result;

h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config) {
  assert(config->identity_user == &runtime);
  ++sdk_calls;
  return sdk_result;
}

const h2_alipay_api_t* h2_esp_alipay_api(void) {
  assert(sdk_result == H2_PAL_OK);
  return &reference;
}

h2_pal_result_t h2_alipay_tiga_protocol_configure(h2_runtime_t* rt,
                                                  const h2_alipay_api_t* sdk) {
  assert(rt == &runtime);
  ++independent_calls;
  observed_reference = sdk;
  return independent_result;
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
  assert(sdk_calls == 0u && independent_calls == 0u);
  sdk_result = H2_PAL_ERR_IO;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_OK);
  assert(sdk_calls == 1u && independent_calls == 1u &&
         observed_reference == NULL);
  sdk_result = H2_PAL_OK;
  independent_result = H2_PAL_ERR_TIMEOUT;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_ERR_TIMEOUT);
  assert(sdk_calls == 2u && independent_calls == 2u &&
         observed_reference == &reference);
  sdk_result = H2_PAL_ERR_IO;
  independent_result = H2_PAL_ERR_INVALID_STATE;
  assert(h2_alipay_e2e_target_configure(&runtime) == H2_PAL_ERR_INVALID_STATE);
  assert(sdk_calls == 3u && independent_calls == 3u &&
         observed_reference == NULL);
  return 0;
}
