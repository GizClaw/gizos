#ifndef H2_TINYH264_ALLOCATOR_BRIDGE_H
#define H2_TINYH264_ALLOCATOR_BRIDGE_H

#include <stddef.h>

void *h2_tinyh264_malloc(size_t size);
void h2_tinyh264_free(void *ptr);

#endif
