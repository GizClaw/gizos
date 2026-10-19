#ifndef H2_POSIX_THREAD_CORE_H
#define H2_POSIX_THREAD_CORE_H

#include "h2_pal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_posix_thread_core h2_posix_thread_core_t;

typedef struct h2_posix_thread_core_resource_stats {
  size_t live_tasks;
  size_t task_stack_bytes;
  size_t live_queues;
  size_t live_mutexes;
  size_t live_semaphores;
  size_t live_conditions;
  size_t live_timers;
} h2_posix_thread_core_resource_stats_t;

/** Create real pthread-backed PAL capabilities. The allocator object is copied;
 * its user/vtable remain borrowed until destroy. Task stacks are allocated with
 * it and passed to pthread_attr_setstack (64 KiB default/minimum, or a larger
 * requested size). These are the running threads' real stacks, not
 * placeholders. The owner must serialize core destruction against new API
 * users. */
h2_pal_result_t
h2_posix_thread_core_create(const h2_pal_mem_api_t *task_allocator,
                            h2_posix_thread_core_t **out);
/** BUSY preserves ownership while any object/start/cleanup is live. */
h2_pal_result_t h2_posix_thread_core_destroy(h2_posix_thread_core_t **core);
const h2_pal_task_api_t *
h2_posix_thread_core_task_api(h2_posix_thread_core_t *core);
const h2_pal_queue_api_t *
h2_posix_thread_core_queue_api(h2_posix_thread_core_t *core);
const h2_pal_sync_api_t *
h2_posix_thread_core_sync_api(h2_posix_thread_core_t *core);
const h2_pal_timer_api_t *
h2_posix_thread_core_timer_api(h2_posix_thread_core_t *core);

/** Cooperative cancellation: never pthread_cancel or force-kill a Worker.
 * Blocking queue/semaphore/mutex/condition operations observe the flag in
 * bounded 10 ms slices. Condition errors/cancel return with the mutex owned.
 * join and timer stop/destroy drain ownership despite cancellation.
 * Join blocks on Workers; on the browser UI thread it tries once and reports
 * BUSY until native pthread termination can be reaped safely. */
h2_pal_result_t h2_posix_thread_core_task_cancel(h2_posix_thread_core_t *core,
                                                 h2_pal_task_t *task);
int h2_posix_thread_core_is_current_task_cancelled(
    h2_posix_thread_core_t *core);
/** UI sleep with nonzero duration is INVALID_STATE; zero is a nonblocking
 * cancellation check. Worker sleep is interruptible by cooperative cancel. */
h2_pal_result_t h2_posix_thread_core_sleep_ms(h2_posix_thread_core_t *core,
                                              uint32_t ms);
/** Atomic individual counters; compare snapshots at quiescent boundaries.
 * Completed tasks remain live until a successful native pthread join. */
h2_pal_result_t h2_posix_thread_core_get_resource_stats(
    h2_posix_thread_core_t *core, h2_posix_thread_core_resource_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif
