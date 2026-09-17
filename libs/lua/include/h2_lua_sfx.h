#ifndef H2_LUA_SFX_H
#define H2_LUA_SFX_H

/** @file h2_lua_sfx.h @brief Named sound effects played by the embedding app. */

#include "h2_lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of names one Host can register. */
#define H2_LUA_SFX_MAX 32u

/**
 * Plays one named effect through the embedding app's own audio path.
 *
 * Called from `audio.play_sfx(name)` on a Lua worker with that job's mutex
 * held. It must return promptly, never block on audio I/O and never call back
 * into the Lua Host; queue the effect to the app's own player instead.
 * @param user Pointer passed at registration.
 * @param app_id App id of the calling job; empty when submitted without one.
 * @param name Registered effect name.
 * Both strings are borrowed for the duration of the call only.
 * @return H2_PAL_OK when queued; H2_PAL_ERR_BUSY or H2_PAL_ERR_WOULD_BLOCK
 * when the player cannot take it now; any other error reports a failure.
 */
typedef h2_pal_result_t (*h2_lua_sfx_play_fn)(void *user, const char *app_id,
                                              const char *name);

/**
 * Registers the handler for `audio.play_sfx(name)`.
 * Register before h2_lua_host_start(); the registry is immutable afterwards.
 * @return H2_PAL_OK; H2_PAL_ERR_INVALID_ARG for a NULL host/handler or an empty
 * or too long name; H2_PAL_ERR_INVALID_STATE after start or for a duplicate
 * name; H2_PAL_ERR_FULL beyond H2_LUA_SFX_MAX names.
 */
h2_pal_result_t h2_lua_register_sfx(h2_lua_host_t *host, const char *name,
                                    h2_lua_sfx_play_fn play, void *user);

#ifdef __cplusplus
}
#endif

#endif
