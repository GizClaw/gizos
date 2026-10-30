#ifndef H2_SZP_CAMERA_TEST_HEAP_H
#define H2_SZP_CAMERA_TEST_HEAP_H
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_DMA 2u
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
#endif
