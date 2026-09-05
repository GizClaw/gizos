#ifndef TEST_SEMPHR_H
#define TEST_SEMPHR_H
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *storage);
SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t max_count, UBaseType_t initial_count, StaticSemaphore_t *storage);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, uint32_t timeout);
#endif
