#ifndef H2_WEB_LUA_APP_H
#define H2_WEB_LUA_APP_H

/**
 * @file h2_web_lua_app.h
 * @brief Optional hooks for pages built by h2_lua_web_app().
 *
 * `h2_lua_web_app()` (lua_web_app.bzl) runs one embedded Lua script on
 * app_host. A consumer that needs extra behaviour passes an `extension`
 * cc_library defining `h2_web_lua_app_extension`; every field is optional and
 * every hook runs on the App task, never on a Lua worker.
 */

#include "h2_lua.h"
#include "h2_runtime.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_web_lua_app_extension {
  /** Registers native modules or capabilities before the Host starts. */
  h2_pal_result_t (*register_host)(h2_lua_host_t *host);
  /** Decides whether one exit-Button event ends the job. NULL ends it on the
   * Button Action that reports the release. */
  bool (*exit_requested)(const h2_runtime_event_t *event);
} h2_web_lua_app_extension_t;

/** Defined by the extension library passed to h2_lua_web_app(). */
extern const h2_web_lua_app_extension_t h2_web_lua_app_extension;

#ifdef __cplusplus
}
#endif

#endif
