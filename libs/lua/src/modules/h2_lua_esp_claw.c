#include "h2_lua_esp_claw.h"

static const h2_lua_esp_claw_module_info_t s_modules[] = {
    {"adc", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"gpio", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"i2c", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"mcpwm", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"pcnt", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"rmt", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"touch", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"uart", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"audio", H2_LUA_ESP_CLAW_MODULE_PROFILE},
    {"board_manager", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"button", H2_LUA_ESP_CLAW_MODULE_COMPONENT_ADAPTED},
    {"ble", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"ble_hid", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"camera", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"capability", H2_LUA_ESP_CLAW_MODULE_FULL},
    {"delay", H2_LUA_ESP_CLAW_MODULE_PROFILE},
    {"display", H2_LUA_ESP_CLAW_MODULE_PROFILE},
    {"environmental_sensor", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"event_publisher", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"http_server", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"json", H2_LUA_ESP_CLAW_MODULE_FULL},
    {"image", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"thread", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"imu", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"ir", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"knob", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"lcd", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"lcd_touch", H2_LUA_ESP_CLAW_MODULE_PROFILE},
    {"ledc", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"led_strip", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"lvgl", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"magnetometer", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"sci", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"storage", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
    {"system", H2_LUA_ESP_CLAW_MODULE_PROFILE},
    {"vision", H2_LUA_ESP_CLAW_MODULE_UNAVAILABLE},
};

static const char *const s_extensions[] = {
    "runtime",
};

size_t h2_lua_esp_claw_module_count(void) {
  return sizeof(s_modules) / sizeof(s_modules[0]);
}

const h2_lua_esp_claw_module_info_t *h2_lua_esp_claw_module_at(size_t index) {
  if (index >= h2_lua_esp_claw_module_count()) {
    return NULL;
  }
  return &s_modules[index];
}

size_t h2_lua_extension_count(void) {
  return sizeof(s_extensions) / sizeof(s_extensions[0]);
}

const char *h2_lua_extension_at(size_t index) {
  return index < h2_lua_extension_count() ? s_extensions[index] : NULL;
}
