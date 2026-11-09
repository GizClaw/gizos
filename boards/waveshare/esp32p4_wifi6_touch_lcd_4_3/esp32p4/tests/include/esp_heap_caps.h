#ifndef H2_P4_TEST_HEAP_H
#define H2_P4_TEST_HEAP_H
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps);
void heap_caps_free(void* ptr);
#endif
