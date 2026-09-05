#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
/* Large enough for the host sync test to embed a pthread-backed semaphore. */
typedef struct {
  int value;
  long long opaque[32];
} StaticSemaphore_t;
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#endif
