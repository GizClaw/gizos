#ifndef ESP_AEC_HEAP_TEST_H
#define ESP_AEC_HEAP_TEST_H
#include "fake_sdk.h"
void *heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, unsigned caps);
void heap_caps_free(void *ptr);
#endif
