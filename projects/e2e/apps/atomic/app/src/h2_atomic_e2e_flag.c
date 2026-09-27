#include "h2_atomic_e2e.h"
#include "h2_atomic_static.h"
#include "h2/pal/core/h2_pal_errors.h"

#include <string.h>

H2_ATOMIC_DEFINE_STATIC(flag, s_flag_a, 0u);
H2_ATOMIC_DEFINE_STATIC(flag, s_flag_b, 0u);

typedef struct flag_worker {
  h2_atomic_flag_t *own;
  h2_atomic_flag_t *shared;
  unsigned iterations;
  unsigned operations;
  unsigned busy_observations;
  const h2_pal_time_api_t *time;
  int (*current_core)(void *);
  void *core_user;
  int core;
} flag_worker_t;

static void flag_worker_entry(void *user) {
  flag_worker_t *worker = user;
  worker->core = worker->current_core == NULL
                     ? -1 : worker->current_core(worker->core_user);
  for (unsigned i = 0u; i < worker->iterations; ++i) {
    /* Separate file-static values must never interfere with one another. */
    if (h2_atomic_flag_test_and_set(worker->own, H2_ATOMIC_ACQUIRE))
      ++worker->busy_observations;
    h2_atomic_flag_clear(worker->own, H2_ATOMIC_RELEASE);
    if (!h2_atomic_flag_test_and_set(worker->shared, H2_ATOMIC_ACQUIRE))
      h2_atomic_flag_clear(worker->shared, H2_ATOMIC_RELEASE);
    ++worker->operations;
    if ((i & 255u) == 0u)
      (void)h2_pal_time_sleep_ms(worker->time, 1u);
  }
}

int h2_atomic_flag_e2e_run(const h2_pal_mem_api_t *mem,
                           const h2_pal_task_api_t *task,
                           const h2_pal_time_api_t *time,
                           unsigned iterations, int (*current_core)(void *),
                           void *core_user,
                           h2_atomic_flag_e2e_result_t *out_result) {
  if (out_result == NULL || mem == NULL || task == NULL || time == NULL ||
      iterations == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  out_result->static_wrapper[0] = (uintptr_t)&s_flag_a;
  out_result->static_wrapper[1] = (uintptr_t)&s_flag_b;
  out_result->static_storage[0] = (uintptr_t)s_flag_a.storage;
  out_result->static_storage[1] = (uintptr_t)s_flag_b.storage;
  if (s_flag_a.storage == s_flag_b.storage)
    return H2_PAL_ERR_INVALID_STATE;
  const bool a_busy = h2_atomic_flag_test_and_set(&s_flag_a, H2_ATOMIC_ACQUIRE);
  const bool b_busy = h2_atomic_flag_test_and_set(&s_flag_b, H2_ATOMIC_ACQUIRE);
  if (!a_busy) h2_atomic_flag_clear(&s_flag_a, H2_ATOMIC_RELEASE);
  if (!b_busy) h2_atomic_flag_clear(&s_flag_b, H2_ATOMIC_RELEASE);
  if (a_busy || b_busy) return H2_PAL_ERR_INVALID_STATE;

  h2_atomic_flag_t *shared = h2_pal_mem_alloc(mem, sizeof(*shared));
  if (shared == NULL) return H2_PAL_ERR_NO_MEMORY;
  *shared = (h2_atomic_flag_t){0};
  if (h2_atomic_flag_init(shared) != H2_ATOMIC_OK) {
    h2_pal_mem_free(mem, shared);
    return H2_PAL_ERR_NO_MEMORY;
  }
  out_result->dynamic_wrapper = (uintptr_t)shared;
  out_result->dynamic_storage = (uintptr_t)shared->storage;
  flag_worker_t *workers = h2_pal_mem_alloc(mem, 2u * sizeof(*workers));
  if (workers == NULL) {
    h2_atomic_flag_destroy(shared);
    h2_pal_mem_free(mem, shared);
    return H2_PAL_ERR_NO_MEMORY;
  }
  workers[0] = (flag_worker_t)
      {.own = &s_flag_a, .shared = shared, .iterations = iterations, .time = time,
       .current_core = current_core, .core_user = core_user, .core = -1};
  workers[1] = (flag_worker_t)
      {.own = &s_flag_b, .shared = shared, .iterations = iterations, .time = time,
       .current_core = current_core, .core_user = core_user, .core = -1};
  h2_pal_task_t *handles[2] = {NULL, NULL};
  int rc = H2_PAL_OK;
  for (unsigned i = 0u; i < 2u; ++i) {
    const h2_pal_task_options_t options = {
        .name = i == 0u ? "atomic/flag/low" : "atomic/flag/high",
        .min_stack_size = 4096u,
    };
    rc = h2_pal_task_start(task, &options, flag_worker_entry, &workers[i],
                           &handles[i]);
    if (rc != H2_PAL_OK) break;
  }
  for (unsigned i = 0u; i < 2u; ++i) {
    if (handles[i] == NULL) continue;
    int join_rc = H2_PAL_ERR_WOULD_BLOCK;
    for (unsigned retry = 0u; retry < 20000u; ++retry) {
      join_rc = h2_pal_task_join(task, handles[i]);
      if (join_rc != H2_PAL_ERR_WOULD_BLOCK && join_rc != H2_PAL_ERR_BUSY)
        break;
      (void)h2_pal_time_sleep_ms(time, 1u);
    }
    if (join_rc != H2_PAL_OK)
      return join_rc; /* Keep heap worker state and shared flag alive. */
  }
  for (unsigned i = 0u; i < 2u; ++i) {
    out_result->operations[i] = workers[i].operations;
    out_result->busy_observations[i] = workers[i].busy_observations;
    out_result->worker_core[i] = workers[i].core;
    if (workers[i].operations != iterations || workers[i].busy_observations)
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_atomic_flag_destroy(shared);
  h2_pal_mem_free(mem, shared);
  h2_pal_mem_free(mem, workers);
  return rc;
}
