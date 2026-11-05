#include "h2_esp_board_battery_curve.h"

#include <stddef.h>

#define H2_ESP_ZERO_BATTERY_CALIBRATION_SCALE 100000
#define H2_ESP_ZERO_BATTERY_CALIBRATION_SLOPE 164775
#define H2_ESP_ZERO_BATTERY_CALIBRATION_INTERCEPT 16402941
#define H2_ESP_ZERO_BATTERY_PERCENT_MAX_X100 10000u

typedef struct h2_esp_zero_esp_battery_curve_point {
  int32_t voltage_mv;
  uint16_t percent_x100;
} h2_esp_zero_esp_battery_curve_point_t;

/* Averaged 5% capacity knots from two 803045-1520 0.5C discharge runs. */
static const h2_esp_zero_esp_battery_curve_point_t s_discharging_curve[] = {
    {2999, 0u},     {3393, 500u},  {3489, 1000u}, {3547, 1500u}, {3593, 2000u},
    {3626, 2500u},  {3649, 3000u}, {3668, 3500u}, {3685, 4000u}, {3705, 4500u},
    {3731, 5000u},  {3759, 5500u}, {3791, 6000u}, {3827, 6500u}, {3871, 7000u},
    {3923, 7500u},  {3973, 8000u}, {4025, 8500u}, {4081, 9000u}, {4145, 9500u},
    {4276, 10000u},
};

/* The voltage-only terminal knot collapses the measured 90-100% CV plateau. */
static const h2_esp_zero_esp_battery_curve_point_t s_charging_curve[] = {
    {3579, 0u},    {3799, 500u},  {3832, 1000u}, {3872, 1500u},  {3901, 2000u},
    {3917, 2500u}, {3928, 3000u}, {3939, 3500u}, {3953, 4000u},  {3973, 4500u},
    {3997, 5000u}, {4026, 5500u}, {4059, 6000u}, {4093, 6500u},  {4134, 7000u},
    {4195, 7500u}, {4246, 8000u}, {4300, 8500u}, {4349, 10000u},
};

int32_t h2_esp_zero_esp_battery_voltage_mv_from_raw(int raw) {
  const int64_t scaled_voltage =
      ((int64_t)H2_ESP_ZERO_BATTERY_CALIBRATION_SLOPE * raw) +
      H2_ESP_ZERO_BATTERY_CALIBRATION_INTERCEPT;
  return (
      int32_t)((scaled_voltage + (H2_ESP_ZERO_BATTERY_CALIBRATION_SCALE / 2)) /
               H2_ESP_ZERO_BATTERY_CALIBRATION_SCALE);
}

static uint16_t interpolate_curve(
    const h2_esp_zero_esp_battery_curve_point_t* curve, size_t point_count,
    int32_t voltage_mv) {
  if (voltage_mv <= curve[0].voltage_mv) {
    return curve[0].percent_x100;
  }

  for (size_t i = 1; i < point_count; ++i) {
    const h2_esp_zero_esp_battery_curve_point_t* low = &curve[i - 1u];
    const h2_esp_zero_esp_battery_curve_point_t* high = &curve[i];
    if (voltage_mv <= high->voltage_mv) {
      const int32_t voltage_span = high->voltage_mv - low->voltage_mv;
      const int32_t voltage_offset = voltage_mv - low->voltage_mv;
      const uint32_t percent_span =
          (uint32_t)high->percent_x100 - low->percent_x100;
      return (uint16_t)(low->percent_x100 +
                        ((int64_t)voltage_offset * percent_span) /
                            voltage_span);
    }
  }

  return curve[point_count - 1u].percent_x100;
}

static uint16_t interpolate_scaled_curve(
    const h2_esp_zero_esp_battery_curve_point_t* curve, size_t point_count,
    int32_t voltage_mv, int32_t product_full_voltage_mv) {
  const int32_t measured_full_voltage_mv = curve[point_count - 1u].voltage_mv;
  int32_t measured_curve_voltage_mv = H2_ESP_ZERO_BATTERY_USABLE_MIN_MV;
  if (voltage_mv >= product_full_voltage_mv) {
    measured_curve_voltage_mv = measured_full_voltage_mv;
  } else if (voltage_mv > H2_ESP_ZERO_BATTERY_USABLE_MIN_MV) {
    measured_curve_voltage_mv =
        H2_ESP_ZERO_BATTERY_USABLE_MIN_MV +
        (int32_t)(((int64_t)(voltage_mv - H2_ESP_ZERO_BATTERY_USABLE_MIN_MV) *
                   (measured_full_voltage_mv -
                    H2_ESP_ZERO_BATTERY_USABLE_MIN_MV)) /
                  (product_full_voltage_mv -
                   H2_ESP_ZERO_BATTERY_USABLE_MIN_MV));
  }
  const uint16_t unusable_percent_x100 =
      interpolate_curve(curve, point_count, H2_ESP_ZERO_BATTERY_USABLE_MIN_MV);
  const uint16_t measured_percent_x100 =
      interpolate_curve(curve, point_count, measured_curve_voltage_mv);
  if (measured_percent_x100 <= unusable_percent_x100) {
    return 0u;
  }
  return (
      uint16_t)(((uint32_t)(measured_percent_x100 - unusable_percent_x100) *
                 H2_ESP_ZERO_BATTERY_PERCENT_MAX_X100) /
                (H2_ESP_ZERO_BATTERY_PERCENT_MAX_X100 - unusable_percent_x100));
}

uint16_t h2_esp_zero_esp_battery_percent_x100(int32_t voltage_mv,
                                              bool charging) {
  if (charging) {
    return interpolate_scaled_curve(
        s_charging_curve,
        sizeof(s_charging_curve) / sizeof(s_charging_curve[0]), voltage_mv,
        H2_ESP_ZERO_BATTERY_CHARGING_FULL_MV);
  }
  return interpolate_scaled_curve(
      s_discharging_curve,
      sizeof(s_discharging_curve) / sizeof(s_discharging_curve[0]), voltage_mv,
      H2_ESP_ZERO_BATTERY_DISCHARGING_FULL_MV);
}
