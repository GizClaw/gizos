#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define configMAX_PRIORITIES 16u
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
#define CONFIG_SPIRAM 1
#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1
#endif
