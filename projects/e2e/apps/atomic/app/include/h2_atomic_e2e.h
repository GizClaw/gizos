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
} h2_atomic_flag_e2e_result_t;

/**
 * @brief Exercise two independent static flags and a dynamic flag.
 *
 * The allocator, task and time APIs are borrowed for this call. The dynamic
 * wrapper uses @p mem; its backing is owned by the linked atomic provider.
 * Calls are serialized because the two static flags live for the process.
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
