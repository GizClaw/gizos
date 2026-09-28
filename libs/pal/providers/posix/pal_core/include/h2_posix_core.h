#ifndef H2_POSIX_CORE_H
#define H2_POSIX_CORE_H
#include "h2_pal.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Provider-private shared native implementation; public PAL ABI stays in
 * libs/pal. */
typedef struct h2_posix_core_resources {
  size_t tasks, task_stack_bytes, queues, mutexes, semaphores, conditions,
      timers;
  size_t allocations, allocation_bytes;
} h2_posix_core_resources_t;
const h2_pal_mem_api_t *h2_posix_core_mem_api(void);
const h2_pal_time_api_t *h2_posix_core_time_api(void);
const h2_pal_task_api_t *h2_posix_core_task_api(void);
const h2_pal_queue_api_t *h2_posix_core_queue_api(void);
const h2_pal_sync_api_t *h2_posix_core_sync_api(void);
const h2_pal_timer_api_t *h2_posix_core_timer_api(void);
const h2_pal_log_api_t *h2_posix_core_log_api(void);
const h2_pal_system_event_api_t *h2_posix_system_event_api(void);
h2_pal_result_t h2_posix_core_resources(h2_posix_core_resources_t *out);
/* Only the real task-stack allocation path is affected. -1 disables faults. */
h2_pal_result_t
h2_posix_core_task_allocation_fault(int successful_before_failure);
/* Call only after all users and Runtime instances have stopped. BUSY keeps
 * ownership. */
h2_pal_result_t h2_posix_core_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
