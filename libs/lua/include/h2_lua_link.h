#ifndef H2_LUA_LINK_H
#define H2_LUA_LINK_H

/** @file h2_lua_link.h @brief BLE KCP peer link for Lua jobs (`link` module).
 */

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2_lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Largest message accepted by link.send() and delivered as LINK_MESSAGE. */
#define H2_LUA_LINK_MESSAGE_MAX 256u
/** Longest session tag accepted by link.host() and link.join(). */
#define H2_LUA_LINK_TAG_MAX 32u

typedef struct h2_lua_link_config {
  /** Advertising PDU family the board BLE stack supports for host(). */
  h2_pal_ble_adv_type_t adv_type;
  /** Scan procedure the board BLE stack supports for join(). */
  h2_pal_ble_scan_type_t scan_type;
} h2_lua_link_config_t;

/**
 * Installs the BLE KCP provider behind the Lua `link` module.
 *
 * Call once after h2_lua_host_create() and before h2_lua_host_start(). The
 * Runtime's BLE Host must already be started by the caller and stay running
 * until h2_lua_host_destroy() returns; the provider never starts or stops it
 * and never uses Wi-Fi or network APIs. Returns H2_PAL_ERR_UNSUPPORTED, and
 * leaves `link` unavailable, when the Runtime has no BLE Host or system-event
 * API or the BLE Host lacks an operation the link needs.
 * h2_lua_host_destroy() joins the provider's tasks and frees it.
 */
h2_pal_result_t h2_lua_link_enable(h2_lua_host_t *host,
                                   const h2_lua_link_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
