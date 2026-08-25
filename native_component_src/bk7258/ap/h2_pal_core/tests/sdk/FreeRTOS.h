#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
typedef int BaseType_t;
typedef struct { int value; } StaticSemaphore_t;
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#endif
