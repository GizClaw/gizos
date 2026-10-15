#include "h2_bk_platform_core.h"
#include "h2_bk_resource_stats_internal.h"
#include <os/os.h>
#include <assert.h>

/* All bookkeeping is fixed internal storage and never allocates. Snapshots
 * span all BK PAL allocators; compare them at quiescent ownership boundaries. */
static size_t objects[H2_BK_RESOURCE_COUNT], stacks, allocations, allocation_bytes;
void h2_bk_resource_acquire(h2_bk_resource_kind_t kind, size_t bytes) {
    uint32_t level = rtos_enter_critical();
    ++objects[kind];
    stacks += bytes;
    rtos_exit_critical(level);
}
void h2_bk_resource_release(h2_bk_resource_kind_t kind, size_t bytes) {
    uint32_t level = rtos_enter_critical();
    assert(objects[kind] != 0 && stacks >= bytes);
    --objects[kind];
    stacks -= bytes;
    rtos_exit_critical(level);
}
void h2_bk_memory_acquire(size_t bytes) {
    uint32_t level = rtos_enter_critical();
    ++allocations;
    allocation_bytes += bytes;
    rtos_exit_critical(level);
}
void h2_bk_memory_release(size_t bytes) {
    uint32_t level = rtos_enter_critical();
    assert(allocations != 0 && allocation_bytes >= bytes);
    --allocations;
    allocation_bytes -= bytes;
    rtos_exit_critical(level);
}
h2_pal_result_t h2_bk_platform_get_resource_stats(h2_bk_platform_resource_stats_t *out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    uint32_t level = rtos_enter_critical();
    *out = (h2_bk_platform_resource_stats_t){
        .live_tasks = objects[H2_BK_RESOURCE_TASK], .task_stack_bytes = stacks,
        .live_queues = objects[H2_BK_RESOURCE_QUEUE],
        .live_mutexes = objects[H2_BK_RESOURCE_MUTEX],
        .live_semaphores = objects[H2_BK_RESOURCE_SEMAPHORE],
        .live_conditions = objects[H2_BK_RESOURCE_CONDITION],
        .live_timers = objects[H2_BK_RESOURCE_TIMER],
        .live_firmware_infos = 1, /* Static BK firmware metadata provider. */
        .allocations = allocations, .allocation_bytes = allocation_bytes,
    };
    rtos_exit_critical(level);
    return H2_PAL_OK;
}
