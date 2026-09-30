#ifndef H2_ATOMIC_E2E_H
#define H2_ATOMIC_E2E_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_atomic_e2e_backend {
  const char *name;
  int (*create)(const h2_pal_mem_api_t *mem, bool psram, void **out_state);
  void (*destroy)(const h2_pal_mem_api_t *mem, bool psram, void *state);
  unsigned (*work)(void *state, unsigned iterations);
  unsigned (*incremented)(const void *state);
  unsigned (*compared)(const void *state);
  void (*addresses)(const void *state, uintptr_t *wrapper,
                    uintptr_t *storage);
} h2_atomic_e2e_backend_t;

typedef struct h2_atomic_e2e_result {
  unsigned expected;
  unsigned incremented;
  unsigned compared;
  uint64_t elapsed_us;
  bool concurrent;
  bool psram;
  unsigned cas_failures;
  int worker_core[2];
  uintptr_t wrapper_address;
  uintptr_t storage_address;
} h2_atomic_e2e_result_t;

typedef struct h2_atomic_flag_e2e_result {
  uintptr_t static_wrapper[2];
  uintptr_t static_storage[2];
  uintptr_t dynamic_wrapper;
  uintptr_t dynamic_storage;
  unsigned operations[2];
  unsigned busy_observations[2];
  int worker_core[2];
  unsigned workers_started, workers_joined;
  int teardown;
} h2_atomic_flag_e2e_result_t;

/**
 * @brief Exercise two independent static flags and a dynamic flag.
 *
 * The allocator, task and time APIs are borrowed for this call. The dynamic
 * wrapper uses @p mem; its backing is owned by the linked atomic provider.
 * Both workers cross a startup barrier before flag operations. Calls are
 * serialized because the two static flags live for the process.
 * @p out_result is cleared first and may contain partial observations on
 * error. A task join failure deliberately retains heap worker state so a
 * still-running task cannot use freed memory.
 *
 * @return H2_PAL_OK when both workers finish every operation without an
 * unexpected claim; otherwise a PAL argument, allocation, task or state error.
 */
int h2_atomic_flag_e2e_run(const h2_pal_mem_api_t *mem,
                           const h2_pal_task_api_t *task,
                           const h2_pal_time_api_t *time,
                           unsigned iterations, int (*current_core)(void *),
                           void *core_user,
                           h2_atomic_flag_e2e_result_t *out_result);

enum { H2_ATOMIC_QUALIFICATION_CASE_COUNT = 28 };
typedef struct h2_atomic_qualification_config {
  const h2_pal_mem_api_t *mem;
  const h2_pal_task_api_t *task;
  const h2_pal_time_api_t *time;
  void (*pump)(void *);
  void *pump_user;
  int (*current_core)(void *);
  void *core_user;
  /* -1 means no CPU pinning requirement. The callback still records identity. */
  int expected_core[2];
  bool require_distinct_workers;
  /* Board qualification checks every backing while it is still live. */
  int (*check_placement)(uintptr_t wrapper, uintptr_t storage,
                         bool static_value, void *user);
  void *placement_user;
} h2_atomic_qualification_config_t;
typedef struct h2_atomic_qualification_case {
  const char *id;
  int rc;
  unsigned status; /* 0 NOT_RUN, 1 PASS, 2 FAIL */
} h2_atomic_qualification_case_t;
typedef struct h2_atomic_qualification_result {
  h2_atomic_qualification_case_t cases[H2_ATOMIC_QUALIFICATION_CASE_COUNT];
  unsigned passed, failed, not_run;
  unsigned workers_started, workers_joined;
  int worker_core[2];
  int teardown;
  bool complete, qualified;
} h2_atomic_qualification_result_t;
/**
 * @brief Run every mandatory Atomic operation and synchronization case.
 *
 * @param config Borrowed Mem/Task/Time APIs and observation callbacks. Calls
 * are serialized because static backing storage has process lifetime.
 * @param result Caller-owned ledger, reset on entry. Partial failure keeps
 * NOT_RUN cases explicit and qualified false.
 *
 * This synchronous function crosses a two-worker startup barrier, waits up to
 * ten seconds per progress deadline, then joins before retiring atomic values.
 * A failed bounded join retains heap state and sets result->teardown; borrowed
 * task/time dependencies must stay alive until all retained workers stop.
 * @return H2_PAL_OK only for complete PASS and cleanup; otherwise an argument,
 * allocation, task, timeout or invalid-state error. Unsupported init cannot
 * qualify. No diagnostics callback owns or frees backing storage.
 */
int h2_atomic_e2e_qualify(const h2_atomic_qualification_config_t *config,
                         h2_atomic_qualification_result_t *result);
/** @brief Emit the immutable caller-owned ledger to the launcher console. */
void h2_atomic_e2e_print(const char *platform, const char *placement,
                         const h2_atomic_qualification_result_t *result);

const h2_atomic_e2e_backend_t *h2_atomic_e2e_h2_backend(void);
/* Deliberate test-only direct C11 comparison, linked separately. */
const h2_atomic_e2e_backend_t *h2_atomic_e2e_c11_backend(void);

int h2_atomic_e2e_run(const h2_pal_mem_api_t *mem,
                      const h2_pal_task_api_t *task,
                      const h2_pal_time_api_t *time,
                      const h2_atomic_e2e_backend_t *backend,
                      unsigned iterations_per_worker,
                      bool concurrent,
                      bool psram,
                      int (*current_core)(void *), void *core_user,
                      void (*pump)(void *), void *pump_user,
                      h2_atomic_e2e_result_t *out_result);

#ifdef __cplusplus
}
#endif
#endif
