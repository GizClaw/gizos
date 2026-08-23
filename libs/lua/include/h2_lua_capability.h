#ifndef H2_LUA_CAPABILITY_H
#define H2_LUA_CAPABILITY_H

/** @file h2_lua_capability.h @brief C capability registry used by
 * capability.call(). */

#include "h2_lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t h2_lua_capability_request_id_t;

typedef h2_pal_result_t (*h2_lua_capability_call_fn)(
    void *user, h2_lua_capability_request_id_t request_id, const char *input,
    const char *options, char *output, size_t output_capacity,
    const char **out_error);

typedef void (*h2_lua_capability_cancel_fn)(
    void *user, h2_lua_capability_request_id_t request_id);

h2_pal_result_t h2_lua_register_capability(h2_lua_host_t *host,
                                           const char *name,
                                           h2_lua_capability_call_fn call,
                                           h2_lua_capability_cancel_fn cancel,
                                           void *user);

/**
 * Completes a call whose begin callback returned H2_PAL_ERR_WOULD_BLOCK.
 *
 * The tuple observed by Lua is exactly `ok, output, error`. A late or duplicate
 * completion is rejected and never resumes a reused job/task generation.
 */
h2_pal_result_t h2_lua_capability_complete(
    h2_lua_host_t *host, h2_lua_capability_request_id_t request_id,
    h2_pal_result_t result, const char *output, const char *error);

#ifdef __cplusplus
}
#endif

#endif
