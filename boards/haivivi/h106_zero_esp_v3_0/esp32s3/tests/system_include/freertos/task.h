#ifndef H2_TEST_SHADOW_FREERTOS_TASK_H
#define H2_TEST_SHADOW_FREERTOS_TASK_H
#include "freertos/FreeRTOS.h"
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
BaseType_t xTaskCreate(TaskFunction_t task, const char* name,
                       uint32_t stack_depth, void* parameters,
                       UBaseType_t priority, TaskHandle_t* created);
#endif
