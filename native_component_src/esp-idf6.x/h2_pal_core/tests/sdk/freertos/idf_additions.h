#ifndef TEST_IDF_ADDITIONS_H
#define TEST_IDF_ADDITIONS_H
#include "freertos/task.h"
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry,
    const char *name, uint32_t stack_size, void *ctx, UBaseType_t priority,
    TaskHandle_t *out_task, BaseType_t core, uint32_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
#endif
