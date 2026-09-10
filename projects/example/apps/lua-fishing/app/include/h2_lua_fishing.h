#ifndef H2_LUA_FISHING_H
#define H2_LUA_FISHING_H

/** @file h2_lua_fishing.h @brief Portable single-Skill Lua Example App. */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_FISHING_COMPONENT_BUTTON 3u
#define H2_LUA_FISHING_COMPONENT_BACK 5u

typedef int (*h2_lua_fishing_should_stop_fn)(void *user);
typedef h2_pal_result_t (*h2_lua_fishing_ready_fn)(void *user);

typedef struct h2_lua_fishing_config {
  const char *scene;
  const char *time_ms;
  const char *rod;
  const char *reel;
  const char *power;
  const char *action;
  const char *brand;
  const char *check;
  h2_runtime_component_id_t back_component_id;
  h2_lua_fishing_should_stop_fn should_stop;
  void *should_stop_user;
  h2_lua_fishing_ready_fn on_ready;
  void *on_ready_user;
} h2_lua_fishing_config_t;

h2_pal_result_t h2_lua_fishing_run(h2_runtime_t *runtime,
                                      const h2_lua_fishing_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
