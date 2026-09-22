#ifndef H2_LVGL_PLATFORM_H
#define H2_LVGL_PLATFORM_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lvgl_platform_config {
    /** Borrowed for every LVGL allocation, including OSAL objects. Keep this
     * API and its backing storage valid and unchanged through lv_deinit(),
     * then call h2_lvgl_platform_deinit() before destroying the allocator. */
    const h2_pal_mem_api_t *allocator;
    const h2_pal_task_api_t *task_api;
    const h2_pal_sync_api_t *sync_api;
    const h2_pal_queue_api_t *queue_api;
    const h2_pal_time_api_t *time_api;
    /** Initial TLSF storage bytes; 0 preserves direct allocator behavior.
     * Enabled chunk sizes must be in [256, TLSF maximum block size / 2].
     * Chunk descriptors/alignment, TLSF control and a PAL mutex cost extra bytes. */
    size_t pool_initial_bytes;
    /** Growth chunk storage bytes; 0 uses pool_initial_bytes. Ignored when
     * pooling is disabled. Requests above half this size bypass the pool. */
    size_t pool_grow_bytes;
} h2_lvgl_platform_config_t;

/** @brief Bind borrowed PAL APIs before lv_init; return 0 on success.
 * Invalid settings or failed pool setup return a negative result and unwind.
 * Rebinding with an active or requested pool requires platform_deinit first.
 * Native direct-to-direct rebinding keeps its prior behavior; release live
 * allocations before changing APIs. Web rejects duplicate bindings.
 * APIs remain valid through lv_deinit
 * and platform_deinit. Lifecycle calls require quiescent consumers/workers.
 * Pool allocator callbacks must not reenter LVGL memory hooks. */
int h2_lvgl_platform_init(const h2_lvgl_platform_config_t *config);
/** @brief Release owned storage and unbind PAL APIs after lv_deinit.
 * Idempotent; never destroys the borrowed providers themselves. */
void h2_lvgl_platform_deinit(void);

/** Pool-only snapshot; direct allocator mode reports all zeros. */
typedef struct h2_lvgl_memory_stats {
    size_t chunks;              /**< Number of owned TLSF chunks. */
    size_t chunk_bytes;         /**< Caller bytes including chunk descriptors/alignment. */
    size_t control_bytes;       /**< Caller bytes for TLSF control. */
    size_t used_bytes;          /**< Used TLSF block sizes, including rounding. */
    size_t largest_free_bytes;  /**< Raw free block size, before alignment cost. */
    size_t direct_blocks;      /**< Live large allocations outside the pool. */
    size_t direct_bytes;       /**< Requested bytes, excluding direct headers. */
} h2_lvgl_memory_stats_t;

/** @brief Write a snapshot, or return a negative result for NULL/lock failure.
 * Returns 0 with zeros when unbound, disabled or after lv_mem_deinit.
 * Walks the pool under its PAL mutex: O(blocks), blocking, task context only.
 * Native hooks are serialized independently of lv_lock; Web is single-thread.
 * Does not include caller heap overhead or PAL mutex storage. Must not race
 * init/deinit. Memory hooks likewise must not be called from an ISR. */
int h2_lvgl_platform_get_memory_stats(h2_lvgl_memory_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif
