#include "h2_esp_h200_battery_profile.h"

#if defined(H2_ESP_H200_BATTERY_PROFILE_ORIGINAL)
static const h2_esp_h200_battery_profile_t s_profile = {
    .name = "original",
    .capacity_mah = 3000u,
    .voltage_min_mv = 3500,
    .voltage_max_mv = 4060,
    .shutdown_mv = 3500,
};
#elif defined(H2_ESP_H200_BATTERY_PROFILE_DONGHUI)
static const h2_esp_h200_battery_profile_t s_profile = {
    .name = "donghui",
    .capacity_mah = 3000u,
    .voltage_min_mv = 3400,
    .voltage_max_mv = 4000,
    .shutdown_mv = 3400,
};
#else
#error "H200 battery profile was not selected"
#endif

const h2_esp_h200_battery_profile_t* h2_esp_h200_selected_battery_profile(
    void) {
  return &s_profile;
}

int32_t h2_esp_h200_battery_voltage_from_raw(int raw) {
  const int64_t sample = raw;
  const int64_t scaled =
      (5043 * sample * sample) + (137713000 * sample) + 42253000000;
  return (int32_t)(scaled / 100000000);
}
