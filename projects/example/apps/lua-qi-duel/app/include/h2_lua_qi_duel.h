#ifndef H2_LUA_QI_DUEL_H
#define H2_LUA_QI_DUEL_H

/** @file h2_lua_qi_duel.h @brief Portable Lua Qi Duel interface App. */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_QI_DUEL_COMPONENT_BACK 5u

/** Cooperative lifecycle callback. A nonzero result requests App exit. */
typedef int (*h2_lua_qi_duel_should_stop_fn)(void *user);

/** Called once after the first complete frame has been presented. */
typedef h2_pal_result_t (*h2_lua_qi_duel_ready_fn)(void *user);

/** Portable Qi Duel execution policy. */
typedef struct h2_lua_qi_duel_config {
  /** Optional mapped Back Button; NONE disables Back cancellation. */
  h2_runtime_component_id_t back_component_id;
  /** Required cooperative stop callback. */
  h2_lua_qi_duel_should_stop_fn should_stop;
  void *should_stop_user;
  /** Optional callback after the first frame and Lua wait boundary. */
  h2_lua_qi_duel_ready_fn on_ready;
  void *on_ready_user;
} h2_lua_qi_duel_config_t;

/** Runs the Qi Duel Lua resource using the supplied initialized Runtime. */
h2_pal_result_t h2_lua_qi_duel_run(h2_runtime_t *runtime,
                                    const h2_lua_qi_duel_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
