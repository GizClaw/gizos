#ifndef H2_ESP_ALIPAY_INTERNAL_H
#define H2_ESP_ALIPAY_INTERNAL_H
#include "h2_esp_alipay.h"
const h2_esp_alipay_config_t* h2_esp_alipay_config(void);
void h2_esp_alipay_trace(const char* operation, const char* boundary,
                         int result, uint32_t item, uint32_t connection,
                         uint64_t started_ms, const char* detail,
                         const void* bytes, size_t length);
uint64_t h2_esp_alipay_now(void);
#endif
