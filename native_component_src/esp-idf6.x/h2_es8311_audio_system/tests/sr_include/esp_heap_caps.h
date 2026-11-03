#ifndef ESP_AEC_HEAP_TEST_H
#define ESP_AEC_HEAP_TEST_H
#include "fake_sdk.h"
void* heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size,
                               unsigned caps);
void heap_caps_free(void* ptr);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
size_t heap_caps_get_minimum_free_size(unsigned caps);
int heap_caps_monitor_local_minimum_free_size_start(void);
int heap_caps_monitor_local_minimum_free_size_stop(void);
#endif
