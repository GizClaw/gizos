#ifndef H2_PAL_CORE_E2E_H
#define H2_PAL_CORE_E2E_H

#include "h2_pal_core_baseline.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_CORE_E2E_CONTRACT_VERSION 2u
#define H2_PAL_CORE_CASE(id) +1
enum {
  H2_PAL_CORE_E2E_CASE_COUNT = 0
#include "h2_pal_core_cases.inc"
};
#undef H2_PAL_CORE_CASE

/** Launcher evidence from actual provider objects and forwarding allocators. */
typedef struct h2_pal_core_resources {
  size_t tasks, task_stack_bytes, queues, mutexes, semaphores, conditions;
  size_t timers, firmware_infos, allocations, allocation_bytes;
} h2_pal_core_resources_t;

typedef enum h2_pal_core_e2e_status {
  H2_PAL_CORE_E2E_NOT_RUN = 0,
  H2_PAL_CORE_E2E_PASS,
  H2_PAL_CORE_E2E_FAIL,
  H2_PAL_CORE_E2E_BLOCKED,
} h2_pal_core_e2e_status_t;

typedef struct h2_pal_core_e2e_case_result {
  const char *id;
  h2_pal_core_e2e_status_t status;
  h2_pal_result_t result;
} h2_pal_core_e2e_case_result_t;

typedef enum h2_pal_core_queue_latest {
  H2_PAL_CORE_QUEUE_LATEST_UNSPECIFIED = 0,
  H2_PAL_CORE_QUEUE_LATEST_REPLACE,
  H2_PAL_CORE_QUEUE_LATEST_FALLBACK,
} h2_pal_core_queue_latest_t;

typedef struct h2_pal_core_e2e_config {
  /** Bounded operation budget, 100..60000 ms. Zero selects 2000 ms. */
  uint32_t timeout_ms;
  /** Public send_latest behavior declared by the platform: replace oldest,
   * or the header's no-wait send fallback. Proxies hide vtable omissions. */
  h2_pal_core_queue_latest_t queue_latest;
  /** Independent launcher clock; never implement using the PAL under test. */
  h2_pal_result_t (*observe_monotonic_us)(void *user, uint64_t *out_us);
  /** Checks actual sink output after a successful PAL write. Missing = BLOCKED.
   */
  h2_pal_result_t (*observe_log)(void *user, h2_pal_log_level_t level,
                                 const char *scope, const char *message);
  void *observer_user;
  /** Optional progress observation before a case; cannot alter its verdict. */
  void (*case_begin)(void *user, const char *case_id);
  /** Explicitly permit changing this provider's wall clock and restoring UTC.
   */
  int allow_wall_set;
  /** Expected current embedded image version, supplied from build metadata. */
  const char *firmware_version;
  /** Exclusive, uninitialized event fixture using the platform's real provider.
   * App owns init/deinit for this fixture only. Never pass the Runtime's shared
   * live event loop. NULL = BLOCKED; runtime->system_event is never modified.
   */
  const h2_pal_system_event_api_t *event_fixture;
  /** Snapshot real resources; required for v2 qualification. */
  h2_pal_result_t (*observe_resources)(void *user,
                                       h2_pal_core_resources_t *out);
  /** Called from the actual worker, returns its native stack size. */
  h2_pal_result_t (*observe_task_stack)(void *user, size_t *out_bytes);
  /** Control an allocator used by the real Task provider. -1 disables faults;
   * N >= 0 rejects the allocation after N successful task-stack allocations.
   * It must not replace start/join with a fake Task implementation. */
  h2_pal_result_t (*task_allocation_fault)(void *user,
                                           int successful_before_failure);
} h2_pal_core_e2e_config_t;

typedef struct h2_pal_core_e2e_owner h2_pal_core_e2e_owner_t;
typedef struct h2_pal_core_e2e_result {
  h2_pal_core_e2e_case_result_t cases[H2_PAL_CORE_E2E_CASE_COUNT];
  size_t passed;
  size_t failed;
  size_t blocked;
  size_t not_run;
  int complete;
  int qualified;
  h2_pal_result_t cleanup_result;
  h2_pal_core_baseline_result_t baseline;
  h2_pal_core_e2e_owner_t *retained_cleanup;
} h2_pal_core_e2e_result_t;

/** Runs every declared case; unavailable capability/evidence is never PASS.
 * Keep runtime, config and result alive until cleanup succeeds. Do not reuse
 * result while it retains resources. Launcher must provide an independent
 * process/device watchdog: PAL join/lock APIs cannot enforce timed returns.
 * Finite testing does not prove all schedules or board memory placements. */
h2_pal_result_t h2_pal_core_e2e_run(h2_runtime_t *runtime,
                                    const h2_pal_core_e2e_config_t *config,
                                    h2_pal_core_e2e_result_t *result);
/** Retry retained cleanup; failed tests remain failed after cleanup recovers.
 */
h2_pal_result_t h2_pal_core_e2e_cleanup(h2_runtime_t *runtime,
                                        h2_pal_core_e2e_result_t *result);
const char *h2_pal_core_e2e_status_name(h2_pal_core_e2e_status_t status);

#ifdef __cplusplus
}
#endif
#endif
