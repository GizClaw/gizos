#ifndef H2_APPLICATION_HOST_LUA_H
#define H2_APPLICATION_HOST_LUA_H

#include "h2_application_host.h"
#include "h2_lua.h"
#include "h2_lua_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One portable Lua job on the shared Application Host. host.runtime is filled
 * from the entry Runtime. Host resources, module contexts and allocator users
 * must be persistent if checked Lua destruction can fail. args are consumed
 * by submit; the runner borrows config itself only for this call. register_host
 * runs before Lua workers start. event may dispatch to the job or set cancel;
 * NULL forwards supported Lua component events and ignores system/custom
 * events. A job reaching its terminal state during a batch is normal. */
typedef struct h2_application_host_lua_config {
    h2_lua_host_config_t host;
    const char *resource;
    const h2_lua_arg_t *args;
    size_t arg_count;
    void *user;
    h2_pal_result_t (*register_host)(void *user, h2_lua_host_t *lua);
    h2_pal_result_t (*event)(void *user, h2_lua_host_t *lua,
                             h2_lua_job_id_t job,
                             const h2_runtime_event_t *event, int *out_cancel);
} h2_application_host_lua_config_t;

/** Run from the App entry task. Cooperative Host stop cancels the job; ready
 * is reported on its first wait. Always perform checked Lua worker/display
 * shutdown before returning. Ordinary job failure destroys Lua and returns an
 * error. An uncertain destroy retains Lua as the Host's lifetime root; callers
 * must preserve Runtime/providers and all borrowed module/resource contexts.
 * Job cancellation is normal completion; script failure is never PASS.
 */
h2_pal_result_t
h2_application_host_lua_run(h2_application_host_t *host, h2_runtime_t *runtime,
                            const h2_application_host_lua_config_t *config);

#ifdef __cplusplus
}
#endif
#endif
