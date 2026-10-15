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
  /** Successful prepare reserves one owned reference for the product.
   * Store it with the reserved job. Required completion makes it runnable on
   * OK; on error the product releases it. After success release only after
   * the Lua Host is actually destroyed, retaining it through failed cleanup.
   * Prepare error transfers no reference; the library releases it. */
  h2_gizclaw_lua_app_lease_t *lease;
  size_t param_count;
  h2_gizclaw_lua_app_param_t params[H2_GIZCLAW_LUA_APP_PARAMS_MAX];
} h2_gizclaw_lua_app_run_t;

/** Required after successful preparation. complete runs exactly once with
 * the response-completion result: OK makes the reserved request runnable;
 * any error releases it without launching. Both callbacks return promptly,
 * never stop/destroy the Service inline, and retain no borrowed request views.
 * The completion user must outlive Service deinit, or the standalone device
 * stop/destroy that settles its local call. */
typedef struct h2_gizclaw_lua_app_handoff {
  h2_gizclaw_rpc_response_complete_fn complete;
  void *user;
} h2_gizclaw_lua_app_handoff_t;

#ifdef __cplusplus
}
#endif
#endif
