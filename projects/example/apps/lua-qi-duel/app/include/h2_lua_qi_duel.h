#ifndef H2_LUA_QI_DUEL_H
#define H2_LUA_QI_DUEL_H

/** @file h2_lua_qi_duel.h @brief Portable Lua Qi Duel interface App. */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_QI_DUEL_COMPONENT_BACK 5u
#define H2_LUA_QI_DUEL_COMPONENT_VOLUME_UP 9u
#define H2_LUA_QI_DUEL_COMPONENT_VOLUME_DOWN 10u
#define H2_LUA_QI_DUEL_COMPONENT_RECORD 11u

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
  /** Optional inspector: NULL/full, individual layers, scene7/scene8. */
  const char *layer;
  /** Optional nonnegative scene time in milliseconds; NULL animates. */
  const char *time_ms;
  /** Optional deterministic health-effect probe: player-down/up, enemy-down/up. */
  const char *health_fx;
  /** Optional deterministic charge-effect probe: down/up. */
  const char *charge_fx;
  /** Optional initial charge value: 0..5; default 3. */
  const char *qi;
  /** Desktop pointer policy: click left/right to rotate, center to cast. */
  int click_controls;
  /** Local simultaneous-round game; zero retains visual rehearsal mode. */
  int battle;
  /** Optional firmware management-advertising ownership hooks; supply both. */
  h2_pal_result_t (*pause_management_advertising)(void *user);
  h2_pal_result_t (*resume_management_advertising)(void *user);
  void *management_advertising_user;
  /* Optional deterministic carousel probes: zero-based selection and offset. */
  const char *selected;
  const char *drag;
  /** Optional action rehearsal/probe: charge, wave, absorb, guard. */
  const char *action;
  /** Optional rehearsal actor: both (default), player, opponent. */
  const char *actor;
  /** Optional impact-label probe: combo or armor-break. */
  const char *impact;
  /** Optional beam-clash VFX probe: equal/player-combo/enemy-combo/both-combo. */
  const char *clash;
  /** Optional settlement probe: win or lose. */
  const char *result;
  /** Explicit desktop component review; NULL keeps only approved replacements. */
  const char *draw_component;
  /** Deterministic sequence capture advances this many milliseconds per frame. */
  const char *capture_step_ms;
} h2_lua_qi_duel_config_t;

/** Runs the Qi Duel Lua resource using the supplied initialized Runtime. */
h2_pal_result_t h2_lua_qi_duel_run(h2_runtime_t *runtime,
                                    const h2_lua_qi_duel_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
