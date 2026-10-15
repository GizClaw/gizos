#include "FreeRTOS.h"
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t task, int slot);
void vTaskSetThreadLocalStoragePointer(TaskHandle_t task, int slot, void *value);
