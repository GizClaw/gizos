#ifndef H2_GIZCLAW_LUA_APPS_H
#define H2_GIZCLAW_LUA_APPS_H
#include "h2_gizclaw_lua_app_catalog.h"
#include "h2_gizclaw_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_LUA_APP_PARAMS_MAX 16u
#define H2_GIZCLAW_LUA_APP_PARAMS_BYTES_MAX 4096u

/** Borrowed string pair, mapped directly to a Lua Host argument. */
typedef struct h2_gizclaw_lua_app_param {
  const char *name;
  const char *value;
} h2_gizclaw_lua_app_param_t;

/** Borrowed only during prepare_lua_app_run. Copy the app snapshot and every
 * parameter string before returning. The product reserves its bounded owner
 * queue before the library acknowledges launch acceptance. */
typedef struct h2_gizclaw_lua_app_run {
  h2_gizclaw_lua_app_info_t app;
  size_t param_count;
  h2_gizclaw_lua_app_param_t params[H2_GIZCLAW_LUA_APP_PARAMS_MAX];
} h2_gizclaw_lua_app_run_t;

/** Required after successful preparation. complete runs exactly once with
 * the response-completion result: OK makes the reserved request runnable;
 * any error releases it without launching. Both callbacks return promptly,
 * never stop/destroy the Service inline, and retain no borrowed request views.
 * The completion user must outlive Service deinit. */
typedef struct h2_gizclaw_lua_app_handoff {
  h2_gizclaw_rpc_response_complete_fn complete;
  void *user;
} h2_gizclaw_lua_app_handoff_t;

#ifdef __cplusplus
}
#endif
#endif
