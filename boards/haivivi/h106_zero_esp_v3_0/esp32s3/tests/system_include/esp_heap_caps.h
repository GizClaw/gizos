#ifndef H2_TEST_SHADOW_ESP_HEAP_CAPS_H
#define H2_TEST_SHADOW_ESP_HEAP_CAPS_H
#include <stddef.h>
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
#endif
