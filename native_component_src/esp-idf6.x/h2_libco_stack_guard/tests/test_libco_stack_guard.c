#include "h2_esp_libco_stack_guard.h"
#include "h2_esp_libco_stack_guard_test_sdk.h"
#include <stdbool.h>
#include <stdio.h>

static StaticTask_t tcb = {(void *)0x1000, (void *)0xcafe, (void *)0x3000};
static int critical;
static bool monitored = true;
static uint32_t active_sp = 0x2000, hardware_low = 0x1000, hardware_high = 0x3000;
static uint32_t watchpoint = 0x1000;
static RvCoprocSaveArea areas[3];
static unsigned allocations;
RvCoprocSaveArea *test_save_area(uint32_t top) {
    if (top == 0x3000) return &areas[0];
    if (top == 0x40003000) return &areas[1];
    assert(top == 0x40007000); return &areas[2];
}
RvCoprocSaveArea *pxPortGetCoprocArea(StaticTask_t *task, bool allocate, int coproc) {
    assert(task == &tcb && coproc >= 0 && coproc < 3);
    RvCoprocSaveArea *sa = test_save_area((uint32_t)(uintptr_t)task->pxDummy8);
    if (!sa->sa_allocator) {
        sa->sa_tcbstack = task->pxDummy6;
        sa->sa_allocator = (uint32_t)(uintptr_t)task->pxDummy6;
    }
    if (allocate && !sa->sa_coprocs[coproc]) {
        assert(sa == &areas[0]); /* Allocations belong to the original task. */
        sa->sa_coprocs[coproc] = (void *)(uintptr_t)sa->sa_allocator;
        sa->sa_allocator += 32u;
        task->pxDummy6 = (void *)(uintptr_t)sa->sa_allocator;
        ++allocations;
    }
    return sa;
}
void *xTaskGetCurrentTaskHandle(void) { return &tcb; }
void test_enter(portMUX_TYPE *lock) { (void)lock; assert(critical == 0); ++critical; }
void test_exit(portMUX_TYPE *lock) { (void)lock; assert(critical == 1); --critical; }
void vPortSetStackWatchpoint(void *stack) {
    assert(critical == 1); watchpoint = (uint32_t)(uintptr_t)stack;
}
void esp_hw_stack_guard_monitor_stop(void) { assert(critical == 1); monitored = false; }
void esp_hw_stack_guard_set_bounds(uint32_t low, uint32_t high) {
    assert(critical == 1 && !monitored);
    hardware_low = low; hardware_high = high;
}
void esp_hw_stack_guard_monitor_start(void) {
    assert(critical == 1 && !monitored);
    assert(active_sp >= hardware_low && active_sp <= hardware_high);
    monitored = true;
}
static void move(uint32_t low, uint32_t high, uint32_t *old_low, uint32_t *old_high) {
    uint32_t before_low = (uint32_t)(uintptr_t)tcb.pxDummy6;
    uint32_t before_high = (uint32_t)(uintptr_t)tcb.pxDummy8;
    uint32_t token = h2_libco_riscv32_stack_switch_enter(low, high, old_low, old_high);
    assert(critical == 1 && *old_low == before_low && *old_high == before_high);
    assert((uintptr_t)tcb.pxDummy6 == low && (uintptr_t)tcb.pxDummy8 == high);
    assert(tcb.owner == (void *)0xcafe && watchpoint == low);
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    assert(!monitored && hardware_low == low && hardware_high == high);
#endif
    RvCoprocSaveArea *sa = test_save_area(high);
    assert(sa->sa_tcbstack == (void *)0x1000 && sa->sa_allocator == 0x1060);
    for (int i = 0; i < 3; ++i) {
        /* A preemption that saves FPU/PIE/HWLOOP finds real task-owned buffers. */
        assert(pxPortGetCoprocArea(&tcb, true, i)->sa_coprocs[i] ==
               (void *)(uintptr_t)(0x1000u + 32u * (unsigned)i));
    }
    assert(allocations == 3 && (uintptr_t)tcb.pxDummy6 == low);
    /* The actual RV32 assembly changes SP here, before calling exit. */
    active_sp = low + (high - low) / 2;
    h2_libco_riscv32_stack_switch_exit(token);
    assert(critical == 0 && monitored);
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    /* Model the SDK interrupt return: it reinstalls bounds from the TCB. */
    hardware_low = (uint32_t)(uintptr_t)tcb.pxDummy6;
    hardware_high = (uint32_t)(uintptr_t)tcb.pxDummy8;
    assert(active_sp >= hardware_low && active_sp <= hardware_high);
#endif
}
int main(void) {
    h2_esp_libco_stack_guard_link();
    uint32_t first_sp = h2_libco_riscv32_stack_prepare(0x40001000, 0x40003000);
    uint32_t second_sp = h2_libco_riscv32_stack_prepare(0x40005000, 0x40007000);
    assert((first_sp & 15u) == 0 && first_sp <= 0x40003000 - sizeof(RvCoprocSaveArea));
    assert((second_sp & 15u) == 0 && second_sp <= 0x40007000 - sizeof(RvCoprocSaveArea));
    for (int i = 0; i < 3; ++i) (void)pxPortGetCoprocArea(&tcb, true, i);
    for (unsigned i = 0; i < 100u; ++i) {
        uint32_t root_low, root_high, first_low, first_high, second_low, second_high;
        move(0x40001000, 0x40003000, &root_low, &root_high);
        move(0x40005000, 0x40007000, &first_low, &first_high);
        move(root_low, root_high, &second_low, &second_high);
        assert(first_low == 0x40001000 && first_high == 0x40003000);
        assert(second_low == 0x40005000 && second_high == 0x40007000);
        assert((uintptr_t)tcb.pxDummy6 == 0x1060 && (uintptr_t)tcb.pxDummy8 == 0x3000);
    }
    puts("S31 stack bounds, interrupt restoration, watchpoint and ownership preserved");
    return 0;
}
