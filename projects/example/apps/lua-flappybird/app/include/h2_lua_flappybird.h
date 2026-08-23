#ifndef H2_LUA_FLAPPYBIRD_H
#define H2_LUA_FLAPPYBIRD_H

/** @file h2_lua_flappybird.h @brief Portable single-Skill Lua Example App. */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_FLAPPYBIRD_COMPONENT_BUTTON 3u
#define H2_LUA_FLAPPYBIRD_COMPONENT_BACK 5u

typedef int (*h2_lua_flappybird_should_stop_fn)(void *user);
typedef h2_pal_result_t (*h2_lua_flappybird_ready_fn)(void *user);

typedef struct h2_lua_flappybird_config {
  h2_runtime_component_id_t button_component_id;
  h2_runtime_component_id_t back_component_id;
  h2_lua_flappybird_should_stop_fn should_stop;
  void *should_stop_user;
  h2_lua_flappybird_ready_fn on_ready;
  void *on_ready_user;
} h2_lua_flappybird_config_t;

h2_pal_result_t h2_lua_flappybird_run(h2_runtime_t *runtime,
                                      const h2_lua_flappybird_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
