#include "h2_atomic_e2e.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <limits.h>
#include <string.h>

typedef struct worker_context {
  const h2_atomic_e2e_backend_t *backend;
  void *state;
  unsigned iterations;
} worker_context_t;

static void worker_entry(void *user) {
  worker_context_t *worker = user;
  worker->backend->work(worker->state, worker->iterations);
}

static int join_worker(const h2_pal_task_api_t *task,
                       const h2_pal_time_api_t *time, h2_pal_task_t *handle,
                       void (*pump)(void *), void *pump_user) {
  for (unsigned retry = 0; retry < 20000u; ++retry) {
    if (pump != NULL) pump(pump_user);
    const int rc = h2_pal_task_join(task, handle);
    if (rc == H2_PAL_OK)
      return rc;
    if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_BUSY)
      return rc;
    (void)h2_pal_time_sleep_ms(time, 1u);
  }
  return H2_PAL_ERR_TIMEOUT;
}

int h2_atomic_e2e_run(const h2_pal_mem_api_t *mem,
                      const h2_pal_task_api_t *task,
                      const h2_pal_time_api_t *time,
                      const h2_atomic_e2e_backend_t *backend,
                      unsigned iterations_per_worker, bool concurrent,
                      void (*pump)(void *), void *pump_user,
                      h2_atomic_e2e_result_t *out_result) {
  if (out_result != NULL)
    memset(out_result, 0, sizeof(*out_result));
  if (mem == NULL || task == NULL || time == NULL || backend == NULL ||
      backend->create == NULL || backend->destroy == NULL ||
      backend->work == NULL || backend->incremented == NULL ||
      backend->compared == NULL || out_result == NULL ||
      iterations_per_worker == 0u || iterations_per_worker > UINT_MAX / 2u)
    return H2_PAL_ERR_INVALID_ARG;

  void *state = NULL;
  int rc = backend->create(mem, &state);
  if (rc != H2_PAL_OK)
    return rc;
  worker_context_t workers[2] = {
      {backend, state, iterations_per_worker},
      {backend, state, iterations_per_worker},
  };
  h2_pal_task_t *handles[2] = {NULL, NULL};
  const h2_pal_task_options_t options = {
      .name = "atomic/e2e", .min_stack_size = 4096u};
  uint64_t start_us = 0u;
  uint64_t end_us = 0u;
  rc = h2_pal_time_get_monotonic_us(time, &start_us);
  for (unsigned i = 0u; i < 2u && rc == H2_PAL_OK; ++i)
    rc = h2_pal_task_start(task, &options, worker_entry, &workers[i], &handles[i]);
  for (unsigned i = 0u; i < 2u; ++i) {
    if (handles[i] != NULL) {
      const int join_rc = join_worker(task, time, handles[i], pump, pump_user);
      if (rc == H2_PAL_OK)
        rc = join_rc;
      if (join_rc != H2_PAL_OK)
        return join_rc; /* Keep worker storage alive if a task remains. */
    }
  }
  if (rc == H2_PAL_OK)
    rc = h2_pal_time_get_monotonic_us(time, &end_us);
  out_result->expected = iterations_per_worker * 2u;
  out_result->incremented = backend->incremented(state);
  out_result->compared = backend->compared(state);
  out_result->elapsed_us = end_us >= start_us ? end_us - start_us : 0u;
  out_result->concurrent = concurrent;
  backend->destroy(mem, state);
  if (rc != H2_PAL_OK)
    return rc;
  return out_result->incremented == out_result->expected &&
                 out_result->compared == out_result->expected
             ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
