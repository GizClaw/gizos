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

h2_pal_result_t
h2_lua_job_submit_text(h2_lua_host_t *host, const char *chunk_name,
                       const uint8_t *source, size_t source_size,
                       const h2_lua_arg_t *args, size_t arg_count,
                       h2_lua_job_id_t *out_job_id);

/** Submits one immutable compiled text resource from Host configuration. */
h2_pal_result_t h2_lua_job_submit_resource(h2_lua_host_t *host,
                                           const char *resource_name,
                                           const h2_lua_arg_t *args,
                                           size_t arg_count,
                                           h2_lua_job_id_t *out_job_id);

/** Loads and submits a confined relative text path through Runtime Filesystem.
 */
h2_pal_result_t h2_lua_job_submit_file(h2_lua_host_t *host,
                                       const char *relative_path,
                                       const h2_lua_arg_t *args,
                                       size_t arg_count,
                                       h2_lua_job_id_t *out_job_id);

h2_pal_result_t h2_lua_job_cancel(h2_lua_host_t *host, h2_lua_job_id_t job_id);

h2_pal_result_t h2_lua_job_get_status(const h2_lua_host_t *host,
                                      h2_lua_job_id_t job_id,
                                      h2_lua_job_status_t *out_status);

h2_pal_result_t h2_lua_job_release(h2_lua_host_t *host, h2_lua_job_id_t job_id);

#ifdef __cplusplus
}
#endif

#endif
