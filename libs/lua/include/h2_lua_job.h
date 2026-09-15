#ifndef H2_LUA_JOB_H
#define H2_LUA_JOB_H

/** @file h2_lua_job.h @brief Isolated Lua job submission and result API. */

#include "h2_lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_lua_job_state {
  H2_LUA_JOB_QUEUED = 0,
  H2_LUA_JOB_RUNNING,
  H2_LUA_JOB_WAITING,
  H2_LUA_JOB_SUCCEEDED,
  H2_LUA_JOB_FAILED,
  H2_LUA_JOB_CANCELLED,
  H2_LUA_JOB_TIMED_OUT,
  H2_LUA_JOB_STOPPED,
} h2_lua_job_state_t;

#define H2_LUA_JOB_MESSAGE_MAX 192u

typedef struct h2_lua_arg {
  const char *name;
  const char *value;
} h2_lua_arg_t;

typedef struct h2_lua_job_status {
  h2_lua_job_state_t state;
  uint64_t resume_count;
  size_t memory_used;
  char message[H2_LUA_JOB_MESSAGE_MAX];
} h2_lua_job_status_t;

/**
 * Every submit takes the app id that scopes the job's `storage` module to
 * `<storage root>/<app_id>/`. An app id is 1..H2_LUA_STORAGE_APP_ID_MAX bytes
 * of `a-z`, `0-9`, `_`, `-` and `.`, not starting with `.`. Jobs sharing an
 * app id share its files. NULL submits a job without storage identity, whose
 * `storage` calls report unavailable.
 */
h2_pal_result_t
h2_lua_job_submit_text(h2_lua_host_t *host, const char *app_id,
                       const char *chunk_name, const uint8_t *source,
                       size_t source_size, const h2_lua_arg_t *args,
                       size_t arg_count, h2_lua_job_id_t *out_job_id);

/** Submits one immutable compiled text resource from Host configuration. */
h2_pal_result_t
h2_lua_job_submit_resource(h2_lua_host_t *host, const char *app_id,
                           const char *resource_name, const h2_lua_arg_t *args,
                           size_t arg_count, h2_lua_job_id_t *out_job_id);

/** Loads and submits a confined relative text path through Runtime Filesystem.
 */
h2_pal_result_t h2_lua_job_submit_file(h2_lua_host_t *host, const char *app_id,
                                       const char *relative_path,
                                       const h2_lua_arg_t *args,
                                       size_t arg_count,
                                       h2_lua_job_id_t *out_job_id);

h2_pal_result_t h2_lua_job_cancel(h2_lua_host_t *host, h2_lua_job_id_t job_id);

h2_pal_result_t h2_lua_job_get_status(const h2_lua_host_t *host,
                                      h2_lua_job_id_t job_id,
                                      h2_lua_job_status_t *out_status);

/**
 * @brief Copies the main chunk's first return value using Lua string conversion.
 * Takes the job mutex (may block); does not allocate or execute Lua. The caller
 * must keep Host alive throughout the call. Release is serialized by this lock.
 * No return: *out_has_result = 0; nil: "nil"; empty string: present, size 0.
 * Tables use __tostring or Lua's default "table: ..." representation.
 * Result storage counts against VM memory and lives until job release.
 * Only SUCCEEDED is readable; other states return INVALID_STATE, unknown or
 * released ids NOT_FOUND (ID_NONE and invalid pointers return INVALID_ARG).
 * out_size is the byte length excluding the terminating NUL. Both output
 * pointers are required and reset to zero on errors, except NO_SPACE reports
 * presence and required length. NULL buffer with capacity 0 queries the size
 * and returns OK. Otherwise capacity must exceed out_size for a present result;
 * NO_SPACE leaves the buffer untouched. Embedded NUL bytes are preserved.
 * Output exceeding output_limit_bytes fails the job with
 * "H2_LUA_VM_OUTPUT_TOO_LARGE"; results are never silently truncated.
 */
h2_pal_result_t h2_lua_job_get_result(const h2_lua_host_t *host,
                                      h2_lua_job_id_t job_id, char *buffer,
                                      size_t capacity, size_t *out_size,
                                      int *out_has_result);

h2_pal_result_t h2_lua_job_release(h2_lua_host_t *host, h2_lua_job_id_t job_id);

#ifdef __cplusplus
}
#endif

#endif
