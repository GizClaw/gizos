#ifndef H2_TEST_SHADOW_ESP_MEMORY_UTILS_H
#define H2_TEST_SHADOW_ESP_MEMORY_UTILS_H
#include <stdbool.h>
bool esp_ptr_internal(const void* p);
bool esp_ptr_dma_capable(const void* p);
#endif
