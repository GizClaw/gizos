#ifndef TEST_SEMPHR_H
#define TEST_SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
#endif
