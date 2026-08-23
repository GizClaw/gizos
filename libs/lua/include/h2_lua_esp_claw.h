#ifndef H2_LUA_ESP_CLAW_H
#define H2_LUA_ESP_CLAW_H

/** @file h2_lua_esp_claw.h @brief Pinned ESP-Claw generic script profile
 * inventory. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_ESP_CLAW_COMMIT "fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1"

typedef enum h2_lua_esp_claw_module_status {
  H2_LUA_ESP_CLAW_MODULE_FULL = 0,
  H2_LUA_ESP_CLAW_MODULE_COMPONENT_ADAPTED,
  H2_LUA_ESP_CLAW_MODULE_PROFILE,
  H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE,
} h2_lua_esp_claw_module_status_t;

typedef struct h2_lua_esp_claw_module_info {
  const char *id;
  h2_lua_esp_claw_module_status_t status;
} h2_lua_esp_claw_module_info_t;

size_t h2_lua_esp_claw_module_count(void);
const h2_lua_esp_claw_module_info_t *h2_lua_esp_claw_module_at(size_t index);
size_t h2_lua_extension_count(void);
const char *h2_lua_extension_at(size_t index);

#ifdef __cplusplus
}
#endif

#endif
