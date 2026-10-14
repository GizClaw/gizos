#pragma once
#include "freertos/FreeRTOS.h"
static inline uint32_t xTaskGetTickCount(void) { return 0; }
static inline void vTaskDelay(uint32_t ms) { (void)ms; }
