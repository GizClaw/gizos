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
 *
 * Returns H2_PAL_OK once the event is queued for the job,
 * H2_PAL_ERR_INVALID_ARG for a malformed event or one whose component kind
 * does not match the Runtime component, H2_PAL_ERR_NOT_FOUND for an unknown
 * or released job, H2_PAL_ERR_CLOSED for a job that already reached a terminal
 * state, and H2_PAL_ERR_FULL when the job already holds
 * `event_delivery_capacity` undelivered events. The event is not queued on any
 * error. A job can reach a terminal state between two events of one Runtime
 * batch, so CLOSED can follow OK for the same job without any other call.
 */
h2_pal_result_t h2_lua_dispatch_runtime_event(h2_lua_host_t *host,
                                              h2_lua_job_id_t job_id,
                                              const h2_runtime_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
