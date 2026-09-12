#ifndef H2_WEB_LUA_APP_H
#define H2_WEB_LUA_APP_H

/**
 * @file h2_web_lua_app.h
 * @brief Optional hooks for pages built by h2_lua_web_app().
 *
 * This header only declares the contract; `:lua_app_extension` has no
 * definition. When h2_lua_web_app() gets `extension = <lib>`, the generated
 * config enables the hooks and exactly that library must define
 * `h2_web_lua_app_extension` (a second definition is a link error). Without an
 * extension the entry never references the symbol. Every hook runs on the App
 * task, never on a Lua worker, and a NULL field keeps the default behaviour.
 */

#include "h2_lua.h"
#include "h2_runtime.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_web_lua_app_extension {
  /** Called once after Host create and before Host start, to register native
   * modules or capabilities. A non-OK result skips start and job submission;
   * the Host is destroyed and the App ends with that result (FAIL). */
  h2_pal_result_t (*register_host)(h2_lua_host_t *host);
  /** Decides cancellation for every Runtime event of the exit Button (Down,
   * Up and each Action) while the job runs; these events never reach the
   * script. True cancels the job once and no further events are offered;
   * false keeps it running. It replaces the default rule, which cancels on
   * the Action that reports the release. */
  bool (*exit_requested)(const h2_runtime_event_t *event);
} h2_web_lua_app_extension_t;

/** Defined by the extension library passed to h2_lua_web_app(). */
extern const h2_web_lua_app_extension_t h2_web_lua_app_extension;

#ifdef __cplusplus
}
#endif

#endif
