#ifndef H2_WEB_LUA_APPS_H
#define H2_WEB_LUA_APPS_H

#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2_lua_apps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_web_lua_apps h2_web_lua_apps_t;

/** Borrowed dependencies; keep them alive until close succeeds. The directory
 * must be the application's existing catalog, including any GizClaw consumer.
 * All mutations use that catalog's installer, limits, lock and filesystem. */
typedef struct h2_web_lua_apps_config {
  h2_lua_apps_t *apps;
  const h2_pal_mem_api_t *allocator;
  const h2_pal_queue_api_t *queue;
  const h2_pal_task_api_t *task;
} h2_web_lua_apps_config_t;

/** Bind globalThis.h2WebLuaApps for this module. Call on a Worker, never the UI
 * thread. One catalog may be bound per module. The JS API offers Promise-based
 * list(), install(Blob|Uint8Array|ArrayBuffer, {sha256?}) and uninstall(appId).
 * list returns the combined visible catalog; install returns the installed
 * app's app_id/version/display_name/description; uninstall resolves undefined.
 * Errors preserve signed PAL codes in error.code. Local concurrent calls are
 * BUSY; the shared catalog also serializes against remote operations.
 *
 * A dedicated Worker feeds at most 32 KiB at a time to begin/write/finish on
 * the same task. Success follows the filesystem's commit barriers; no archive
 * is written to storage. Persistence and eviction follow the supplied FS.
 * Owners must coordinate mutation with running users of app entry/data paths.
 *
 * On failure out is normally NULL. If rollback cannot release resources it
 * retains a non-NULL handle; close it before releasing any dependencies. */
h2_pal_result_t h2_web_lua_apps_open(const h2_web_lua_apps_config_t *config,
                                     h2_web_lua_apps_t **out);

/** Stop accepting JS calls, reject pending Promises, abort an unfinished
 * install on its owning Worker, and join before releasing the bridge.
 * Call on a Worker, never the UI thread. NULL is a no-op. Failure retains the
 * handle/dependencies for a later close retry; the JS API remains closed.
 * Call before destroying the borrowed catalog or filesystem. */
h2_pal_result_t h2_web_lua_apps_close(h2_web_lua_apps_t **bridge);

#ifdef __cplusplus
}
#endif
#endif
