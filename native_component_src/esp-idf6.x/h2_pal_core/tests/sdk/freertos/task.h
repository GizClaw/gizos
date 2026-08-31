#ifndef TEST_TASK_H
#define TEST_TASK_H
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define tskNO_AFFINITY ((BaseType_t)0x7fffffff)
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name,
    uint32_t stack_size, void *ctx, UBaseType_t priority,
    TaskHandle_t *out_task, BaseType_t core);
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t entry,
    const char *name, uint32_t stack_size, void *ctx,
    UBaseType_t priority, StackType_t *stack, StaticTask_t *task_storage,
    BaseType_t core);
StackType_t *xTaskGetStackStart(TaskHandle_t task);
void vTaskDelete(TaskHandle_t task);
#endif
