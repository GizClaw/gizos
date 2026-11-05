#ifndef H2_TEST_SHADOW_FREERTOS_H
#define H2_TEST_SHADOW_FREERTOS_H
#include <stdint.h>
#include <assert.h>
typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS 1
#define configMAX_PRIORITIES 25
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0u
static inline void test_port_enter_critical(portMUX_TYPE* lock) {
  assert(*lock == 0u);
  *lock = 1u;
}
static inline void test_port_exit_critical(portMUX_TYPE* lock) {
  assert(*lock == 1u);
  *lock = 0u;
}
#define portENTER_CRITICAL(lock) test_port_enter_critical(lock)
#define portEXIT_CRITICAL(lock) test_port_exit_critical(lock)
#define portENTER_CRITICAL_ISR(lock) test_port_enter_critical(lock)
#define portEXIT_CRITICAL_ISR(lock) test_port_exit_critical(lock)
#endif
