#ifndef H2_ALIPAY_SDK_INTERNAL_H
#define H2_ALIPAY_SDK_INTERNAL_H
#include "h2_alipay_sdk.h"
const h2_alipay_sdk_config_t* h2_alipay_sdk_config(void);
void h2_alipay_sdk_trace(const char* operation, const char* boundary,
                         int result, uint32_t item, uint32_t connection,
                         uint64_t started_ms, const char* detail,
                         const void* bytes, size_t length);
uint64_t h2_alipay_sdk_now(void);
#endif
