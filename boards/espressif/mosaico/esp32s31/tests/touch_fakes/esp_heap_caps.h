#ifndef H2_MOSAICO_TOUCH_TEST_HEAP_H
#define H2_MOSAICO_TOUCH_TEST_HEAP_H
#include <stdlib.h>
#define MALLOC_CAP_DMA 1u
#define MALLOC_CAP_INTERNAL 2u
#define MALLOC_CAP_SPIRAM 4u
#define MALLOC_CAP_8BIT 8u
static inline void* heap_caps_malloc(size_t size, unsigned int caps) {
  (void)caps;
  return malloc(size);
}
static inline void* heap_caps_calloc(size_t count, size_t size,
                                     unsigned int caps) {
  (void)caps;
  return calloc(count, size);
}
static inline void heap_caps_free(void* memory) { free(memory); }
#endif
