#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef unsigned char StackType_t;
typedef uint32_t TickType_t;
#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000u
#endif
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) * configTICK_RATE_HZ / 1000u))
typedef struct { unsigned char storage[64]; } StaticSemaphore_t;
typedef struct { unsigned char storage[64]; } StaticTask_t;
typedef int portMUX_TYPE;
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#ifndef portMAX_DELAY
#define portMAX_DELAY 0xffffffffu
#endif
#define configMAX_PRIORITIES 16u
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
#define CONFIG_SPIRAM 1
#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
BaseType_t xPortInIsrContext(void);
#endif
