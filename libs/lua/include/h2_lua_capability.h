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

/**
 * Register an exact name (copied, 1..47 bytes) before start. Call is required;
 * cancel is optional and user is borrowed until destruction. Serialize with
 * registration/start/destruction. Duplicate names or a frozen registry return
 * INVALID_STATE, invalid arguments INVALID_ARG, and capacity exhaustion FULL.
 * The callback runs on the owning worker and may return WOULD_BLOCK for later
 * completion. Exact matches take precedence over all prefix registrations.
 */
h2_pal_result_t h2_lua_register_capability(h2_lua_host_t *host,
                                           const char *name,
                                           h2_lua_capability_call_fn call,
                                           h2_lua_capability_cancel_fn cancel,
                                           void *user);

/**
 * Prefix callback receives the full requested name, borrowed for this call.
 * Other arguments and WOULD_BLOCK/completion/cancel semantics match exact
 * calls. Copy name, input and options before returning if asynchronous work
 * needs them.
 */
typedef h2_pal_result_t (*h2_lua_capability_prefix_call_fn)(
    void *user, h2_lua_capability_request_id_t request_id, const char *name,
    const char *input, const char *options, char *output,
    size_t output_capacity, const char **out_error);

/**
 * Register a namespace before start; serialize registration/start/destruction.
 * Prefix is copied, must contain 1..47 bytes (less than internal
 * H2_LUA_NAME_MAX). Call is required; cancel is optional; user is borrowed
 * until host destruction. Exact matches win, otherwise the longest prefix with
 * a nonempty remainder wins. The same text may be registered once per mode.
 * Duplicate prefixes or a frozen registry return INVALID_STATE; invalid
 * arguments return INVALID_ARG; shared capability_capacity exhaustion returns
 * FULL. Does not invoke the callback.
 */
h2_pal_result_t
h2_lua_register_capability_prefix(h2_lua_host_t *host, const char *prefix,
                                  h2_lua_capability_prefix_call_fn call,
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
