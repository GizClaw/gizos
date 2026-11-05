#ifndef H2_TEST_SEMPHR_H
#define H2_TEST_SEMPHR_H
#include <stdint.h>
typedef int StaticSemaphore_t;
typedef void* SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage);
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout);
void xSemaphoreGive(SemaphoreHandle_t mutex);
#endif
