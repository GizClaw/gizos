#ifndef H2_ESP_H200_BATTERY_PROFILE_H
#define H2_ESP_H200_BATTERY_PROFILE_H

#include <stdint.h>

typedef struct h2_esp_h200_battery_profile {
  const char* name;
  uint16_t capacity_mah;
  int32_t voltage_min_mv;
  int32_t voltage_max_mv;
  int32_t shutdown_mv;
} h2_esp_h200_battery_profile_t;

const h2_esp_h200_battery_profile_t* h2_esp_h200_selected_battery_profile(void);
int32_t h2_esp_h200_battery_voltage_from_raw(int raw);

#endif
