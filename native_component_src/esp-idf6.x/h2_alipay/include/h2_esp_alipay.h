#ifndef H2_ESP_ALIPAY_H
#define H2_ESP_ALIPAY_H
#include "h2_alipay_sdk.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Source-compatible names. PAL APIs are borrowed until hardware reset. */
typedef h2_alipay_sdk_storage_t h2_esp_alipay_secure_t;
typedef h2_alipay_sdk_config_t h2_esp_alipay_config_t;
/** Configure the generic SDK bridge, defaulting read_mac to ESP_MAC_BT. */
h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config);
const h2_alipay_api_t* h2_esp_alipay_api(void);
#ifdef __cplusplus
}
#endif
#endif
