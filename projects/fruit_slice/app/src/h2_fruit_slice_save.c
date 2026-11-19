#include "h2_fruit_slice_save.h"

#include <string.h>

static int close_namespace(h2_pal_pref_namespace_t* ns, int result) {
  if (ns != NULL && ns->close != NULL) {
    const int closed = ns->close(ns);
    if (result == H2_PAL_OK) result = closed;
  }
  return result;
}

int h2_fruit_slice_save_best(h2_runtime_t* runtime, uint32_t best) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns == NULL) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    rc = ns->set_u32 != NULL
             ? ns->set_u32(ns, H2_FRUIT_SLICE_PREF_BEST_KEY, best)
             : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK && ns->commit != NULL) rc = ns->commit(ns);
  }
  return close_namespace(ns, rc);
}

int h2_fruit_slice_save_blob(h2_runtime_t* runtime, const char* key,
                             const void* data, size_t size) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(runtime->pref, H2_FRUIT_SLICE_PREF_NAMESPACE,
                            H2_PAL_PREF_OPEN_READ_WRITE, &ns);
  if (rc == H2_PAL_OK && ns == NULL) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    rc = ns->set_blob != NULL ? ns->set_blob(ns, key, data, size)
                              : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK && ns->commit != NULL) rc = ns->commit(ns);
  }
  return close_namespace(ns, rc);
}

static void save_snapshot(void* user) {
  h2_fruit_slice_save_worker_t* w = user;
  h2_fruit_slice_save_completion_t* result = &w->completion;
  const h2_fruit_slice_save_snapshot_t* snapshot = &result->snapshot;
  uint64_t started = 0u, ended = 0u;
  const int timed = h2_pal_time_get_monotonic_ms(w->runtime->time, &started);
  if (snapshot->fields & H2_FRUIT_SLICE_SAVE_BEST)
    result->best_result = h2_fruit_slice_save_best(w->runtime, snapshot->best);
  if (snapshot->fields & H2_FRUIT_SLICE_SAVE_PROGRESS)
    result->progress_result = h2_fruit_slice_save_blob(
        w->runtime, H2_FRUIT_SLICE_PROGRESS_KEY, snapshot->progress,
        sizeof(snapshot->progress));
  if (snapshot->fields & H2_FRUIT_SLICE_SAVE_CARDS)
    result->cards_result =
        h2_fruit_slice_save_blob(w->runtime, H2_FRUIT_SLICE_CARDS_KEY,
                                 snapshot->cards, sizeof(snapshot->cards));
  if (timed == H2_PAL_OK &&
      h2_pal_time_get_monotonic_ms(w->runtime->time, &ended) == H2_PAL_OK &&
      ended >= started) {
    const uint64_t elapsed = ended - started;
    result->work_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
  }
  /* There is no further persistence or application work after publication. */
  h2_atomic_bool_store(&w->done, true, H2_ATOMIC_RELEASE);
}

int h2_fruit_slice_save_worker_init(h2_fruit_slice_save_worker_t* w,
                                    h2_runtime_t* runtime) {
  memset(w, 0, sizeof(*w));
  if (runtime->task == NULL || runtime->task->vtable == NULL ||
      runtime->task->vtable->start == NULL ||
      runtime->task->vtable->join == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  if (h2_atomic_bool_init(&w->done, false) != H2_ATOMIC_OK)
    return H2_PAL_ERR_NO_MEMORY;
  w->runtime = runtime;
  w->initialized = true;
  return H2_PAL_OK;
}

int h2_fruit_slice_save_worker_start(
    h2_fruit_slice_save_worker_t* w,
    const h2_fruit_slice_save_snapshot_t* snapshot) {
  if (!w->initialized) return H2_PAL_ERR_INVALID_STATE;
  if (w->task != NULL) return H2_PAL_ERR_BUSY;
  if (snapshot->fields == 0u) return H2_PAL_ERR_INVALID_ARG;
  w->completion = (h2_fruit_slice_save_completion_t){.snapshot = *snapshot};
  h2_atomic_bool_store(&w->done, false, H2_ATOMIC_RELEASE);
  const h2_pal_task_options_t options = {.name = "fruit-slice/save",
                                         .min_stack_size = 8192u};
  return h2_pal_task_start(w->runtime->task, &options, save_snapshot, w,
                           &w->task);
}

int h2_fruit_slice_save_worker_poll(
    h2_fruit_slice_save_worker_t* w, bool wait,
    h2_fruit_slice_save_completion_t* completion, bool* completed) {
  *completed = false;
  if (w->task == NULL) return H2_PAL_OK;
  if (!wait && !h2_atomic_bool_load(&w->done, H2_ATOMIC_ACQUIRE))
    return H2_PAL_OK;
  const int rc = h2_pal_task_join(w->runtime->task, w->task);
  if (rc != H2_PAL_OK) return rc;
  w->task = NULL;
  *completion = w->completion;
  *completed = true;
  return H2_PAL_OK;
}

void h2_fruit_slice_save_worker_destroy(h2_fruit_slice_save_worker_t* w) {
  if (w->task != NULL) return;
  if (w->initialized) h2_atomic_bool_destroy(&w->done);
  memset(w, 0, sizeof(*w));
}
