#ifndef H2_ESP_LIBCO_STACK_GUARD_TEST_SDK_H
#define H2_ESP_LIBCO_STACK_GUARD_TEST_SDK_H
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CONFIG_IDF_TARGET_ARCH_RISCV 1
#ifndef SOC_CPU_COPROC_NUM
#define SOC_CPU_COPROC_NUM 3
#endif
#define CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK 1
#define configASSERT assert
#define SOC_CPU_HAS_FPU (SOC_CPU_COPROC_NUM > 0)
#define SOC_CPU_HAS_PIE (SOC_CPU_COPROC_NUM > 0)
#define SOC_CPU_HAS_HWLOOP (SOC_CPU_COPROC_NUM > 0)
typedef struct { uint32_t sa_enable; void *sa_tcbstack; uint32_t sa_allocator; void *sa_coprocs[3]; } RvCoprocSaveArea;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
typedef struct { void *pxDummy6; void *owner; void *pxDummy8; } StaticTask_t;
void *xTaskGetCurrentTaskHandle(void);
RvCoprocSaveArea *test_save_area(uint32_t top);
RvCoprocSaveArea *pxPortGetCoprocArea(StaticTask_t *task, bool allocate, int coproc);
void test_enter(portMUX_TYPE *lock);
void test_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL test_enter
#define portEXIT_CRITICAL test_exit
void vPortSetStackWatchpoint(void *stack);
void esp_hw_stack_guard_monitor_stop(void);
void esp_hw_stack_guard_monitor_start(void);
void esp_hw_stack_guard_set_bounds(uint32_t low, uint32_t high);
#endif
