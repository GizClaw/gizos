#ifndef H2_LUA_LINK_E2E_H
#define H2_LUA_LINK_E2E_H

/** @file h2_lua_link_e2e.h @brief Two-board Lua link exercise. */

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lua_link_e2e_config {
  /** "host" advertises, "join" scans; the two boards pick opposite roles. */
  const char *role;
  h2_pal_ble_adv_type_t adv_type;
  h2_pal_ble_scan_type_t scan_type;
  /** Nonzero stays connected at 10 Hz until the link drops and reports how
   * fast the drop surfaced, instead of running the transfer suite. */
  int hold;
} h2_lua_link_e2e_config_t;

/**
 * Runs the link script on a started BLE Host and logs `LINK ...` lines and a
 * final `H2_LUA_LINK_E2E result=...` line through the Runtime Log. Blocks
 * until the script ends.
 */
h2_pal_result_t h2_lua_link_e2e_run(h2_runtime_t *runtime,
                                    const h2_lua_link_e2e_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
