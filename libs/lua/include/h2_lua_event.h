#ifndef H2_LUA_EVENT_H
#define H2_LUA_EVENT_H

/** @file h2_lua_event.h @brief App-owned Runtime event dispatch into one Lua
 * job. */

#include "h2_lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Copies one already-consumed Runtime event into the named live job.
 *
 * This function never calls h2_runtime_poll_event() or h2_runtime_wait_notify().
 */
h2_pal_result_t h2_lua_dispatch_runtime_event(h2_lua_host_t *host,
                                              h2_lua_job_id_t job_id,
                                              const h2_runtime_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
