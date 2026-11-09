#ifndef TEST_TASK_H
#define TEST_TASK_H
#include "FreeRTOS.h"
#define tskNO_AFFINITY (-1)
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t, const char*,
                                           uint32_t, void*, UBaseType_t,
                                           StackType_t*, StaticTask_t*,
                                           BaseType_t);
void vTaskSuspend(TaskHandle_t);
void vTaskDelete(TaskHandle_t);
#endif
