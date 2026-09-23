#include "h2_atomic_e2e.h"
#include "h2_atomic.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <limits.h>
#include <string.h>

typedef struct worker_context {
  const h2_atomic_e2e_backend_t *backend;
  void *state;
  unsigned iterations;
  h2_atomic_uint_t *ready;
  h2_atomic_bool_t *go;
  const h2_pal_time_api_t *time;
  int (*current_core)(void *);
  void *core_user;
  int observed_core;
  unsigned cas_failures;
} worker_context_t;

static void worker_entry(void *user) {
  worker_context_t *worker = user;
  worker->observed_core = worker->current_core == NULL
                              ? -1 : worker->current_core(worker->core_user);
  if (worker->ready != NULL) {
    (void)h2_atomic_uint_fetch_add(worker->ready, 1u, H2_ATOMIC_SEQ_CST);
    while (!h2_atomic_bool_load(worker->go, H2_ATOMIC_ACQUIRE))
      (void)h2_pal_time_sleep_ms(worker->time, 1u);
  }
  worker->cas_failures = worker->backend->work(worker->state,
                                                worker->iterations);
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
                      bool psram, int (*current_core)(void *), void *core_user,
                      void (*pump)(void *), void *pump_user,
                      h2_atomic_e2e_result_t *out_result) {
  if (out_result != NULL)
    memset(out_result, 0, sizeof(*out_result));
  if (mem == NULL || task == NULL || time == NULL || backend == NULL ||
      backend->create == NULL || backend->destroy == NULL ||
      backend->work == NULL || backend->incremented == NULL ||
      backend->compared == NULL || backend->addresses == NULL ||
      out_result == NULL || iterations_per_worker == 0u ||
      iterations_per_worker > UINT_MAX / 2u)
    return H2_PAL_ERR_INVALID_ARG;

  void *state = NULL;
  int rc = backend->create(mem, psram, &state);
  if (rc != H2_PAL_OK)
    return rc;
  h2_atomic_uint_t ready = {0};
  h2_atomic_bool_t go = {0};
  if (concurrent && (h2_atomic_uint_init(&ready, 0u) != H2_ATOMIC_OK ||
                     h2_atomic_bool_init(&go, false) != H2_ATOMIC_OK)) {
    h2_atomic_uint_destroy(&ready);
    h2_atomic_bool_destroy(&go);
    backend->destroy(mem, psram, state);
    return H2_PAL_ERR_NO_MEMORY;
  }
  worker_context_t workers[2] = {
      {.backend = backend, .state = state, .iterations = iterations_per_worker,
       .ready = concurrent ? &ready : NULL, .go = concurrent ? &go : NULL,
       .time = time, .current_core = current_core, .core_user = core_user,
       .observed_core = -1},
      {.backend = backend, .state = state, .iterations = iterations_per_worker,
       .ready = concurrent ? &ready : NULL, .go = concurrent ? &go : NULL,
       .time = time, .current_core = current_core, .core_user = core_user,
       .observed_core = -1},
  };
  h2_pal_task_t *handles[2] = {NULL, NULL};
  uint64_t start_us = 0u;
  uint64_t end_us = 0u;
  if (!concurrent)
    rc = h2_pal_time_get_monotonic_us(time, &start_us);
  for (unsigned i = 0u; i < 2u && rc == H2_PAL_OK; ++i) {
    const h2_pal_task_options_t options = {
        .name = current_core == NULL ? "atomic/e2e"
                                     : (i == 0u ? "atomic/e2e/core0"
                                                : "atomic/e2e/core1"),
        .min_stack_size = 4096u,
    };
    rc = h2_pal_task_start(task, &options, worker_entry, &workers[i],
                           &handles[i]);
  }
  if (concurrent && rc == H2_PAL_OK) {
    for (unsigned retry = 0u; retry < 5000u; ++retry) {
      if (h2_atomic_uint_load(&ready, H2_ATOMIC_SEQ_CST) == 2u)
        break;
      if (pump != NULL) pump(pump_user);
      (void)h2_pal_time_sleep_ms(time, 1u);
    }
    if (h2_atomic_uint_load(&ready, H2_ATOMIC_SEQ_CST) != 2u)
      rc = H2_PAL_ERR_TIMEOUT;
    if (rc == H2_PAL_OK)
      rc = h2_pal_time_get_monotonic_us(time, &start_us);
  }
  if (concurrent)
    h2_atomic_bool_store(&go, true, H2_ATOMIC_RELEASE);
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
  out_result->psram = psram;
  out_result->cas_failures = workers[0].cas_failures + workers[1].cas_failures;
  out_result->worker_core[0] = workers[0].observed_core;
  out_result->worker_core[1] = workers[1].observed_core;
  backend->addresses(state, &out_result->wrapper_address,
                     &out_result->storage_address);
  h2_atomic_uint_destroy(&ready);
  h2_atomic_bool_destroy(&go);
  backend->destroy(mem, psram, state);
  if (rc != H2_PAL_OK)
    return rc;
  return out_result->incremented == out_result->expected &&
                 out_result->compared == out_result->expected &&
                 out_result->cas_failures == 0u &&
                 (current_core == NULL ||
                  (out_result->worker_core[0] == 0 &&
                   out_result->worker_core[1] == 1))
             ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
