#ifndef H2_LUA_FISHING_GAME_H
#define H2_LUA_FISHING_GAME_H

/** @file h2_lua_fishing_game.h @brief Portable single-Skill Lua Example App. */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_FISHING_GAME_COMPONENT_BUTTON 3u
#define H2_LUA_FISHING_GAME_COMPONENT_BACK 5u
#define H2_LUA_FISHING_GAME_COMPONENT_VOL_UP 9u
#define H2_LUA_FISHING_GAME_COMPONENT_VOL_DOWN 10u
#define H2_LUA_FISHING_GAME_COMPONENT_RECORD 11u
#define H2_LUA_FISHING_GAME_COMPONENT_LEFT 12u
#define H2_LUA_FISHING_GAME_COMPONENT_RIGHT 13u

typedef int (*h2_lua_fishing_game_should_stop_fn)(void *user);
typedef h2_pal_result_t (*h2_lua_fishing_game_ready_fn)(void *user);

typedef struct h2_lua_fishing_game_config {
  const char *profile;
  const char *weather;
  const char *hour;
  const char *scene;
  const char *time_ms;
  const char *rod;
  const char *reel;
  const char *lure;
  const char *detail;
  const char *fish_kg;
  const char *power;
  const char *action;
  const char *brand;
  const char *check;
  const char *no_cache;
  const char *scroll;
  const char *layout; /* "h106" for square 2x2 layout; NULL defaults to AMOLED. */
  const char *controls; /* "buttons" enables H106 controls without a touch provider. */
  int input_test; /* Explicit diagnostic opt-in; production leaves this zero. */
  size_t vm_memory_limit_bytes; /* Zero retains the embedded 4 MiB default. */
  h2_runtime_component_id_t back_component_id;
  h2_lua_fishing_game_should_stop_fn should_stop;
  void *should_stop_user;
  h2_lua_fishing_game_ready_fn on_ready;
  void *on_ready_user;
} h2_lua_fishing_game_config_t;

h2_pal_result_t h2_lua_fishing_game_run(h2_runtime_t *runtime,
                                      const h2_lua_fishing_game_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
