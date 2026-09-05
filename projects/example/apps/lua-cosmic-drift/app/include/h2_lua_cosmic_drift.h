#ifndef H2_LUA_COSMIC_DRIFT_H
#define H2_LUA_COSMIC_DRIFT_H

/** @file h2_lua_cosmic_drift.h @brief Portable Lua cosmic evolution App. */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_COSMIC_DRIFT_COMPONENT_BACK 5u

/** Cooperative lifecycle callback. A nonzero result requests App exit. */
typedef int (*h2_lua_cosmic_drift_should_stop_fn)(void *user);

/** Called once after the first complete frame has been presented. */
typedef h2_pal_result_t (*h2_lua_cosmic_drift_ready_fn)(void *user);

/** Portable Cosmic Drift execution policy. */
typedef struct h2_lua_cosmic_drift_config {
  /** Optional mapped Back Button; NONE disables Back cancellation. */
  h2_runtime_component_id_t back_component_id;
  /** Nonzero keeps the latest pointer position active without a press. */
  int hover_control;
  /** Required cooperative stop callback. */
  h2_lua_cosmic_drift_should_stop_fn should_stop;
  void *should_stop_user;
  /** Optional callback after the first frame and Lua wait boundary. */
  h2_lua_cosmic_drift_ready_fn on_ready;
  void *on_ready_user;
} h2_lua_cosmic_drift_config_t;

/**
 * Runs the Cosmic Drift Lua resource using the supplied initialized Runtime.
 *
 * Display, Touch, Memory, monotonic Time, Task, Queue, Sync, and Log are
 * required. The App borrows the Runtime, consumes its event queue, owns one
 * Lua host/job, and releases all job resources before returning.
 */
h2_pal_result_t
h2_lua_cosmic_drift_run(h2_runtime_t *runtime,
                        const h2_lua_cosmic_drift_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
