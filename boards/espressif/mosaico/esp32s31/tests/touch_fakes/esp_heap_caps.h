#ifndef H2_MOSAICO_TOUCH_TEST_HEAP_H
#define H2_MOSAICO_TOUCH_TEST_HEAP_H
#include <stdlib.h>
#define MALLOC_CAP_DMA 1u
#define MALLOC_CAP_INTERNAL 2u
#define MALLOC_CAP_SPIRAM 4u
#define MALLOC_CAP_8BIT 8u
#ifdef H2_MOSAICO_DISPLAY_TEST_ALLOCATOR
void* h2_mosaico_test_dma_alloc(size_t size, unsigned int caps);
#endif
static inline void* heap_caps_malloc(size_t size, unsigned int caps) {
#ifdef H2_MOSAICO_DISPLAY_TEST_ALLOCATOR
  return h2_mosaico_test_dma_alloc(size, caps);
#else
  (void)caps;
  return malloc(size);
#endif
}
static inline void* heap_caps_calloc(size_t count, size_t size,
                                     unsigned int caps) {
  (void)caps;
  return calloc(count, size);
}
static inline void heap_caps_free(void* memory) { free(memory); }
#endif
