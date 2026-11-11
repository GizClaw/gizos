#ifndef H2_MOSAICO_TOUCH_TEST_SEMAPHORE_H
#define H2_MOSAICO_TOUCH_TEST_SEMAPHORE_H
#include <stdint.h>
typedef int StaticSemaphore_t;
typedef StaticSemaphore_t* SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t* storage) {
  return storage;
}
static inline int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t ticks) {
  (void)semaphore;
  (void)ticks;
  return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t semaphore) {
  (void)semaphore;
  return 1;
}
#endif
