#ifndef H2_ALIPAY_TEST_ESP_ALIPAY_H
#define H2_ALIPAY_TEST_ESP_ALIPAY_H

#include "h2_alipay_sdk.h"

typedef h2_alipay_sdk_config_t h2_esp_alipay_config_t;

h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config);
const h2_alipay_api_t* h2_esp_alipay_api(void);

#endif
