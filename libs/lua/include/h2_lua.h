#ifndef H2_LUA_H
#define H2_LUA_H

/**
 * @file h2_lua.h
 * @brief Firmwares Runtime host for isolated Lua jobs.
 */

#include "h2_lua_vm.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lua_host h2_lua_host_t;
typedef uint32_t h2_lua_job_id_t;

#define H2_LUA_JOB_ID_NONE 0u

typedef struct h2_lua_resource {
  const char *name;
  const uint8_t *source;
  size_t source_size;
} h2_lua_resource_t;

/** Longest storage root, app id and storage file name, in bytes. */
#define H2_LUA_STORAGE_ROOT_MAX 96u
#define H2_LUA_STORAGE_APP_ID_MAX 32u
#define H2_LUA_STORAGE_NAME_MAX 32u
/** Upper bound for h2_lua_storage_config_t.app_max_files. */
#define H2_LUA_STORAGE_MAX_FILES 64u

/**
 * Backing for the per-app `storage` Lua module.
 *
 * A job submitted with an app id reads and writes only files directly under
 * `<root>/<app_id>/`. A NULL fs leaves storage unconfigured: `storage` still
 * loads, and its calls return `nil, "storage: unavailable"`.
 */
typedef struct h2_lua_storage_config {
  /** Borrowed filesystem; it must outlive the Host. Needs mkdir, open, read,
   * write, close, stat, remove and a replacing rename. */
  const h2_pal_fs_api_t *fs;
  /** Directory in the fs namespace, for example "/data/lua". Created on the
   * first write when missing. Copied at create; no trailing '/'. */
  const char *root;
  /** Bytes of file content one app may keep. Zero selects 64 KiB. */
  size_t app_quota_bytes;
  /** Files one app may keep, at most H2_LUA_STORAGE_MAX_FILES. Zero selects
   * 16. */
  size_t app_max_files;
} h2_lua_storage_config_t;

typedef struct h2_lua_host_config {
  h2_runtime_t *runtime;
  size_t worker_count;
  size_t worker_stack_size;
  size_t max_jobs;
  size_t event_delivery_capacity;
  size_t callback_capacity_per_job;
  size_t audio_track_capacity_per_job;
  size_t max_coroutines_per_vm;
  size_t ready_queue_capacity;
  size_t waiter_capacity;
  size_t pending_capability_capacity;
  size_t vm_memory_limit_bytes;
  size_t source_limit_bytes;
  size_t output_limit_bytes;
  uint32_t instruction_quantum;
  uint32_t resume_time_budget_ms;
  uint32_t execution_timeout_ms;
  int32_t utc_offset_minutes;
  const h2_lua_resource_t *resources;
  size_t resource_count;
  /** Nonzero borrows an already-open Display without calling PAL open/close.
   * The caller must serialize display-using jobs and suspend other writers
   * before submission. Keep Display alive until job release or Host stop/join
   * and destruction finish. Lua deinit only frees its framebuffer in this mode.
   * Zero preserves the default job-owned open/close lifecycle. */
  int borrow_display;
  /** Per-app persistent storage; zero-initialized leaves it unconfigured. */
  h2_lua_storage_config_t storage;
} h2_lua_host_config_t;

/** Creates a stopped Host that borrows, but never consumes or destroys,
 * Runtime. */
h2_pal_result_t h2_lua_host_create(const h2_lua_host_config_t *config,
                                   h2_lua_host_t **out_host);

/** Freezes module/capability registration and starts accepting jobs. */
h2_pal_result_t h2_lua_host_start(h2_lua_host_t *host);

/** Requests cancellation of live jobs. Safe to repeat. */
h2_pal_result_t h2_lua_host_stop(h2_lua_host_t *host);

/** Joins every Runtime worker after stop. Safe after a successful join. */
h2_pal_result_t h2_lua_host_join(h2_lua_host_t *host);

/** Wakes Runtime workers so they can advance runnable VMs. */
h2_pal_result_t h2_lua_host_step(h2_lua_host_t *host);

/** Releases Host-owned jobs and storage; the borrowed Runtime remains alive. */
void h2_lua_host_destroy(h2_lua_host_t *host);

#ifdef __cplusplus
}
#endif

#endif
