#ifndef H2_ALIPAY_TEST_ESP_RANDOM_H
#define H2_ALIPAY_TEST_ESP_RANDOM_H
#include <stddef.h>
#include <stdint.h>
void esp_fill_random(void* buffer, size_t size);
uint32_t esp_random(void);
#endif
