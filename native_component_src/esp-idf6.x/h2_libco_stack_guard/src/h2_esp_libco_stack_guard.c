#include "h2_esp_libco_stack_guard.h"
#include <stdbool.h>
#include <string.h>
#ifdef H2_ESP_LIBCO_STACK_GUARD_HOST_TEST
#include "h2_esp_libco_stack_guard_test_sdk.h"
#else
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S31
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "riscv/rvruntime-frames.h"
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
#include "esp_private/hw_stack_guard.h"
#endif
#endif
#endif

/* Force this object into the native link so its S31 hooks override weak defaults. */
void h2_esp_libco_stack_guard_link(void) {
    __asm__ volatile ("" ::: "memory");
}

#if CONFIG_IDF_TARGET_ESP32S31
static portMUX_TYPE switch_lock = portMUX_INITIALIZER_UNLOCKED;

/* The pinned FreeRTOS port locates task coprocessor bookkeeping below
 * pxEndOfStack. Reserve it above usable SP and retain the task-owned save
 * buffers when its active stack changes. Bounds alone are insufficient. */
#ifndef H2_ESP_LIBCO_STACK_GUARD_HOST_TEST
extern RvCoprocSaveArea *pxPortGetCoprocArea(StaticTask_t *task, bool allocate, int coproc);
#endif
static RvCoprocSaveArea *save_area(uint32_t top) {
#ifdef H2_ESP_LIBCO_STACK_GUARD_HOST_TEST
    return test_save_area(top);
#else
    return (RvCoprocSaveArea *)(uintptr_t)((top - sizeof(RvCoprocSaveArea)) & ~(uintptr_t)15u);
#endif
}
uint32_t h2_libco_riscv32_stack_prepare(uint32_t stack_min, uint32_t stack_max) {
    uint32_t sp = (stack_max - sizeof(RvCoprocSaveArea)) & ~15u;
    configASSERT(stack_min < sp);
    memset(save_area(stack_max), 0, sizeof(RvCoprocSaveArea));
    return sp;
}

uint32_t h2_libco_riscv32_stack_switch_enter(
    uint32_t next_min, uint32_t next_max,
    uint32_t *previous_min, uint32_t *previous_max) {
    configASSERT(previous_min != NULL && previous_max != NULL);
    configASSERT(next_min != 0 && next_min < next_max);
    portENTER_CRITICAL(&switch_lock);
    StaticTask_t *tcb = (StaticTask_t *)xTaskGetCurrentTaskHandle();
    /* Allocate every supported coprocessor buffer in the original task stack
     * before replacing its bounds. Copies share these task-owned buffers; a
     * lazy interrupt save must never treat coroutine bytes as bookkeeping. */
#if SOC_CPU_HAS_FPU
    (void)pxPortGetCoprocArea(tcb, true, 0);
#endif
#if SOC_CPU_HAS_PIE
    (void)pxPortGetCoprocArea(tcb, true, 1);
#endif
#if SOC_CPU_HAS_HWLOOP
    (void)pxPortGetCoprocArea(tcb, true, 2);
#endif
    RvCoprocSaveArea *previous_save = save_area((uint32_t)(uintptr_t)tcb->pxDummy8);
    RvCoprocSaveArea *next_save = save_area(next_max);
    if (previous_save != next_save)
        memcpy(next_save, previous_save, sizeof(*next_save));
    /* Same accounting fields used by the pinned SDK's shared-stack helper.
     * Updating the TCB also preserves these bounds across timer interrupts. */
    *previous_min = (uint32_t)(uintptr_t)tcb->pxDummy6;
    *previous_max = (uint32_t)(uintptr_t)tcb->pxDummy8;
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    esp_hw_stack_guard_monitor_stop();
#endif
    tcb->pxDummy6 = (void *)(uintptr_t)next_min;
    tcb->pxDummy8 = (void *)(uintptr_t)next_max;
#if CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK
    vPortSetStackWatchpoint((void *)(uintptr_t)next_min);
#endif
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    esp_hw_stack_guard_set_bounds(next_min, next_max);
#endif
    return 0;
}

void h2_libco_riscv32_stack_switch_exit(uint32_t token) {
    (void)token;
    /* Assembly has already installed the destination SP. Retain the hardware
     * monitor and end-of-stack watchpoint instead of disabling protection. */
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    esp_hw_stack_guard_monitor_start();
#endif
    portEXIT_CRITICAL(&switch_lock);
}
#endif
