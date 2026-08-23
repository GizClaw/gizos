#ifndef ESP_HEAP_CAPS_TEST_STUB_H
#define ESP_HEAP_CAPS_TEST_STUB_H

#include <stddef.h>

#define MALLOC_CAP_SPIRAM 0x01u
#define MALLOC_CAP_8BIT 0x02u

void *heap_caps_aligned_calloc(
    size_t alignment,
    size_t count,
    size_t size,
    unsigned caps);
void heap_caps_free(void *memory);

#endif
