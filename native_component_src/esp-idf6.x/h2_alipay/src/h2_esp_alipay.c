#include "h2_esp_alipay.h"
#include "esp_mac.h"

static h2_pal_result_t read_mac(void* user, uint8_t out[6]) {
  (void)user;
  return esp_read_mac(out, ESP_MAC_BT) == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}
h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config) {
  if (config == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_alipay_sdk_config_t sdk = *config;
  if (sdk.read_mac == NULL) sdk.read_mac = read_mac;
  return h2_alipay_sdk_configure(&sdk);
}
const h2_alipay_api_t* h2_esp_alipay_api(void) { return h2_alipay_sdk_api(); }
