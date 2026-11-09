#ifndef IMAGE_TEST_ESP_HEAP_CAPS_H
#define IMAGE_TEST_ESP_HEAP_CAPS_H
#include <stddef.h>
#define MALLOC_CAP_8BIT 2u
#define MALLOC_CAP_INTERNAL 4u
void* heap_caps_calloc(size_t count, size_t size, unsigned caps);
void* heap_caps_malloc(size_t size, unsigned caps);
void heap_caps_free(void* pointer);
#endif
