#include "h2_esp_platform_core.h"
#include "h2_esp_resource_stats_internal.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>

/* All bookkeeping is fixed internal storage and never allocates. Snapshots
 * span all ESP PAL allocators; compare them at quiescent ownership boundaries. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static size_t objects[H2_ESP_RESOURCE_COUNT], stacks, allocations, allocation_bytes;
void h2_esp_resource_acquire(h2_esp_resource_kind_t kind, size_t bytes) {
    portENTER_CRITICAL(&lock);
    ++objects[kind];
    stacks += bytes;
    portEXIT_CRITICAL(&lock);
}
void h2_esp_resource_release(h2_esp_resource_kind_t kind, size_t bytes) {
    portENTER_CRITICAL(&lock);
    assert(objects[kind] != 0 && stacks >= bytes);
    --objects[kind];
    stacks -= bytes;
    portEXIT_CRITICAL(&lock);
}
void h2_esp_memory_acquire(size_t bytes) {
    portENTER_CRITICAL(&lock);
    ++allocations;
    allocation_bytes += bytes;
    portEXIT_CRITICAL(&lock);
}
void h2_esp_memory_release(size_t bytes) {
    portENTER_CRITICAL(&lock);
    assert(allocations != 0 && allocation_bytes >= bytes);
    --allocations;
    allocation_bytes -= bytes;
    portEXIT_CRITICAL(&lock);
}
h2_pal_result_t h2_esp_platform_get_resource_stats(h2_esp_platform_resource_stats_t *out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    *out = (h2_esp_platform_resource_stats_t){
        .live_tasks = objects[H2_ESP_RESOURCE_TASK], .task_stack_bytes = stacks,
        .live_queues = objects[H2_ESP_RESOURCE_QUEUE],
        .live_mutexes = objects[H2_ESP_RESOURCE_MUTEX],
        .live_semaphores = objects[H2_ESP_RESOURCE_SEMAPHORE],
        .live_conditions = objects[H2_ESP_RESOURCE_CONDITION],
        .live_timers = objects[H2_ESP_RESOURCE_TIMER],
        .live_firmware_infos = 1, /* Static ESP app-descriptor provider. */
        .allocations = allocations, .allocation_bytes = allocation_bytes,
    };
    portEXIT_CRITICAL(&lock);
    return H2_PAL_OK;
}
