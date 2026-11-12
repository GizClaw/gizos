#include "h2_g99_storage_worker.h"

#include "h2_g99_task_names.h"

#include <string.h>

struct h2_g99_storage {
  h2_runtime_t* runtime;
  h2_g99_storage_api_t api;
  h2_pal_mutex_t* mutex;
  h2_pal_semaphore_t* wake;
  h2_pal_task_t* task;
  bool stop;
  h2_g99_storage_snapshot_t snapshot;
};

static void validate(h2_g99_storage_volume_t* volume) {
  if (volume->result == H2_PAL_OK &&
      (volume->total_bytes == 0u || volume->used_bytes > volume->total_bytes)) {
    volume->result = H2_PAL_ERR_INVALID_STATE;
  }
  if (volume->result != H2_PAL_OK)
    volume->used_bytes = volume->total_bytes = 0u;
}

static void worker(void* user) {
  h2_g99_storage_t* storage = user;
  h2_runtime_t* runtime = storage->runtime;
  for (;;) {
    if (h2_pal_semaphore_take(runtime->sync, storage->wake, UINT32_MAX) !=
        H2_PAL_OK)
      return;
    if (h2_pal_mutex_lock(runtime->sync, storage->mutex) != H2_PAL_OK) return;
    const bool stop = storage->stop;
    (void)h2_pal_mutex_unlock(runtime->sync, storage->mutex);
    if (stop) return;
    h2_g99_storage_volume_t internal = {.kind = H2_G99_STORAGE_INTERNAL_FLASH,
                                        .result = H2_PAL_ERR_UNSUPPORTED};
    h2_g99_storage_volume_t external = {.kind = storage->api.external_kind,
                                        .result = H2_PAL_ERR_UNSUPPORTED};
    const h2_pal_result_t rc =
        storage->api.query(storage->api.user, &internal, &external);
    if (rc != H2_PAL_OK) internal.result = external.result = rc;
    internal.kind = H2_G99_STORAGE_INTERNAL_FLASH;
    external.kind = storage->api.external_kind;
    validate(&internal);
    validate(&external);
    if (h2_pal_mutex_lock(runtime->sync, storage->mutex) != H2_PAL_OK) return;
    storage->snapshot.internal = internal;
    storage->snapshot.external = external;
    storage->snapshot.pending = false;
    ++storage->snapshot.version;
    (void)h2_pal_mutex_unlock(runtime->sync, storage->mutex);
  }
}

h2_pal_result_t h2_g99_storage_create(h2_runtime_t* runtime,
                                      const h2_g99_storage_api_t* api,
                                      h2_g99_storage_t** out_storage) {
  if (runtime == NULL || out_storage == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_storage = NULL;
  h2_g99_storage_t* storage = h2_pal_mem_alloc(runtime->mem, sizeof(*storage));
  if (storage == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(storage, 0, sizeof(*storage));
  storage->runtime = runtime;
  if (api != NULL) storage->api = *api;
  storage->snapshot.version = 1u;
  storage->snapshot.simulated = storage->api.simulated;
  storage->snapshot.internal = (h2_g99_storage_volume_t){
      .kind = H2_G99_STORAGE_INTERNAL_FLASH, .result = H2_PAL_ERR_UNSUPPORTED};
  storage->snapshot.external = (h2_g99_storage_volume_t){
      .kind = storage->api.external_kind, .result = H2_PAL_ERR_UNSUPPORTED};
  if (storage->api.query == NULL) {
    *out_storage = storage;
    return H2_PAL_OK;
  }
  const h2_pal_mutex_config_t mutex_config = {"g99-storage", runtime->mem, 0u};
  const h2_pal_semaphore_config_t wake_config = {"g99-storage-wake",
                                                 runtime->mem, 0u, 1u};
  h2_pal_result_t rc =
      h2_pal_mutex_create(runtime->sync, &mutex_config, &storage->mutex);
  if (rc == H2_PAL_OK)
    rc = h2_pal_semaphore_create(runtime->sync, &wake_config, &storage->wake);
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t options = {H2_G99_STORAGE_TASK_NAME_VALUE,
                                           16384u};
    rc = h2_pal_task_start(runtime->task, &options, worker, storage,
                           &storage->task);
  }
  if (rc != H2_PAL_OK) {
    const h2_pal_result_t cleanup = h2_g99_storage_destroy(&storage);
    if (cleanup != H2_PAL_OK) *out_storage = storage;
    return cleanup == H2_PAL_OK ? rc : cleanup;
  }
  *out_storage = storage;
  return H2_PAL_OK;
}

h2_pal_result_t h2_g99_storage_request(h2_g99_storage_t* storage) {
  if (storage == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (storage->task == NULL) return H2_PAL_ERR_UNSUPPORTED;
  h2_runtime_t* runtime = storage->runtime;
  h2_pal_result_t rc = h2_pal_mutex_lock(runtime->sync, storage->mutex);
  if (rc != H2_PAL_OK) return rc;
  if (storage->snapshot.pending || storage->stop) {
    (void)h2_pal_mutex_unlock(runtime->sync, storage->mutex);
    return H2_PAL_ERR_BUSY;
  }
  storage->snapshot.pending = true;
  ++storage->snapshot.version;
  rc = h2_pal_semaphore_give(runtime->sync, storage->wake);
  if (rc != H2_PAL_OK) storage->snapshot.pending = false;
  (void)h2_pal_mutex_unlock(runtime->sync, storage->mutex);
  return rc;
}

h2_pal_result_t h2_g99_storage_get(h2_g99_storage_t* storage,
                                   h2_g99_storage_snapshot_t* out) {
  if (storage == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (storage->mutex == NULL) {
    *out = storage->snapshot;
    return H2_PAL_OK;
  }
  const h2_pal_result_t rc =
      h2_pal_mutex_lock(storage->runtime->sync, storage->mutex);
  if (rc != H2_PAL_OK) return rc;
  *out = storage->snapshot;
  return h2_pal_mutex_unlock(storage->runtime->sync, storage->mutex);
}

h2_pal_result_t h2_g99_storage_destroy(h2_g99_storage_t** owner) {
  if (owner == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_g99_storage_t* storage = *owner;
  if (storage == NULL) return H2_PAL_OK;
  h2_runtime_t* runtime = storage->runtime;
  h2_pal_result_t rc;
  if (storage->task != NULL) {
    rc = h2_pal_mutex_lock(runtime->sync, storage->mutex);
    if (rc != H2_PAL_OK) return rc;
    storage->stop = true;
    (void)h2_pal_mutex_unlock(runtime->sync, storage->mutex);
    rc = h2_pal_semaphore_give(runtime->sync, storage->wake);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_FULL) return rc;
    rc = h2_pal_task_join(runtime->task, storage->task);
    if (rc != H2_PAL_OK) return rc;
    storage->task = NULL;
  }
  if (storage->wake != NULL) {
    rc = h2_pal_semaphore_destroy(runtime->sync, storage->wake);
    if (rc != H2_PAL_OK) return rc;
    storage->wake = NULL;
  }
  if (storage->mutex != NULL) {
    rc = h2_pal_mutex_destroy(runtime->sync, storage->mutex);
    if (rc != H2_PAL_OK) return rc;
    storage->mutex = NULL;
  }
  h2_pal_mem_free(runtime->mem, storage);
  *owner = NULL;
  return H2_PAL_OK;
}
