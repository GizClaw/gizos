#ifndef TEST_ESP_HEAP_CAPS_H
#define TEST_ESP_HEAP_CAPS_H
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
#define MALLOC_CAP_INTERNAL 4u
#define MALLOC_CAP_DMA 8u
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned int caps);
size_t heap_caps_get_allocated_size(void *ptr);
void *heap_caps_malloc(size_t size, unsigned int caps);
void *heap_caps_realloc(void *ptr, size_t size, unsigned int caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
#endif
