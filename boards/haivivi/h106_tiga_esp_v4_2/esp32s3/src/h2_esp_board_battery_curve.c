#include "h2_esp_board_battery_curve.h"

#include <stddef.h>

#define H2_ESP_TIGA_BATTERY_CALIBRATION_SCALE 100000
#define H2_ESP_TIGA_BATTERY_CALIBRATION_SLOPE 164775
#define H2_ESP_TIGA_BATTERY_CALIBRATION_INTERCEPT 16402941
#define H2_ESP_TIGA_BATTERY_PERCENT_MAX_X100 10000u

typedef struct h2_esp_tiga_battery_curve_point {
  int32_t discharging_voltage_mv;
  int32_t charging_voltage_mv;
  uint16_t reference_percent_x100;
} h2_esp_tiga_battery_curve_point_t;

/*
 * Common reference-capacity knots. Keep the original 803045-1520 0.5C
 * discharge column and its product-voltage/usable-capacity scaling unchanged.
 * The charging column is already in calibrated product mV: fit stable Tiga
 * state-change windows against that discharge reference, not stored percent.
 * Both columns use the same usable-capacity normalization below. The first
 * two reference knots lie below displayed zero; the charging floor is 3558 mV.
 */
static const h2_esp_tiga_battery_curve_point_t s_battery_curve[] = {
    {2999, 3496, 0u},    {3393, 3535, 500u},  {3489, 3574, 1000u},
    {3547, 3620, 1500u}, {3593, 3656, 2000u}, {3626, 3684, 2500u},
    {3649, 3703, 3000u}, {3668, 3718, 3500u}, {3685, 3732, 4000u},
    {3705, 3749, 4500u}, {3731, 3768, 5000u}, {3759, 3789, 5500u},
    {3791, 3812, 6000u}, {3827, 3838, 6500u}, {3871, 3870, 7000u},
    {3923, 3909, 7500u}, {3973, 3946, 8000u}, {4025, 3987, 8500u},
    {4081, 4031, 9000u}, {4145, 4081, 9500u}, {4276, 4200, 10000u},
};

int32_t h2_esp_tiga_battery_voltage_mv_from_raw(int raw) {
  const int64_t scaled_voltage =
      ((int64_t)H2_ESP_TIGA_BATTERY_CALIBRATION_SLOPE * raw) +
      H2_ESP_TIGA_BATTERY_CALIBRATION_INTERCEPT;
  return (
      int32_t)((scaled_voltage + (H2_ESP_TIGA_BATTERY_CALIBRATION_SCALE / 2)) /
               H2_ESP_TIGA_BATTERY_CALIBRATION_SCALE);
}

static int32_t curve_voltage_mv(const h2_esp_tiga_battery_curve_point_t* point,
                                bool charging) {
  return charging ? point->charging_voltage_mv : point->discharging_voltage_mv;
}

static uint16_t interpolate_curve(int32_t voltage_mv, bool charging) {
  const size_t point_count =
      sizeof(s_battery_curve) / sizeof(s_battery_curve[0]);
  if (voltage_mv <= curve_voltage_mv(&s_battery_curve[0], charging)) {
    return s_battery_curve[0].reference_percent_x100;
  }

  for (size_t i = 1; i < point_count; ++i) {
    const h2_esp_tiga_battery_curve_point_t* low = &s_battery_curve[i - 1u];
    const h2_esp_tiga_battery_curve_point_t* high = &s_battery_curve[i];
    const int32_t low_voltage_mv = curve_voltage_mv(low, charging);
    const int32_t high_voltage_mv = curve_voltage_mv(high, charging);
    if (voltage_mv <= high_voltage_mv) {
      const int32_t voltage_span = high_voltage_mv - low_voltage_mv;
      const int32_t voltage_offset = voltage_mv - low_voltage_mv;
      const uint32_t percent_span =
          (uint32_t)high->reference_percent_x100 - low->reference_percent_x100;
      /* All operands are nonnegative; division truncates toward zero. */
      return (uint16_t)(low->reference_percent_x100 +
                        ((int64_t)voltage_offset * percent_span) /
                            voltage_span);
    }
  }

  return s_battery_curve[point_count - 1u].reference_percent_x100;
}

static uint16_t usable_percent_x100(uint16_t reference_percent_x100) {
  const uint16_t unusable_percent_x100 =
      interpolate_curve(H2_ESP_TIGA_BATTERY_USABLE_MIN_MV, false);
  if (reference_percent_x100 <= unusable_percent_x100) {
    return 0u;
  }
  return (
      uint16_t)(((uint32_t)(reference_percent_x100 - unusable_percent_x100) *
                 H2_ESP_TIGA_BATTERY_PERCENT_MAX_X100) /
                (H2_ESP_TIGA_BATTERY_PERCENT_MAX_X100 - unusable_percent_x100));
}

uint16_t h2_esp_tiga_battery_percent_x100(int32_t voltage_mv, bool charging) {
  if (charging) {
    if (voltage_mv <= H2_ESP_TIGA_BATTERY_CHARGING_MIN_MV) {
      return 0u;
    }
    if (voltage_mv >= H2_ESP_TIGA_BATTERY_CHARGING_FULL_MV) {
      return H2_ESP_TIGA_BATTERY_PERCENT_MAX_X100;
    }
    return usable_percent_x100(interpolate_curve(voltage_mv, true));
  }

  const size_t point_count =
      sizeof(s_battery_curve) / sizeof(s_battery_curve[0]);
  const int32_t measured_full_voltage_mv =
      s_battery_curve[point_count - 1u].discharging_voltage_mv;
  int32_t measured_curve_voltage_mv = H2_ESP_TIGA_BATTERY_USABLE_MIN_MV;
  if (voltage_mv >= H2_ESP_TIGA_BATTERY_DISCHARGING_FULL_MV) {
    measured_curve_voltage_mv = measured_full_voltage_mv;
  } else if (voltage_mv > H2_ESP_TIGA_BATTERY_USABLE_MIN_MV) {
    measured_curve_voltage_mv =
        H2_ESP_TIGA_BATTERY_USABLE_MIN_MV +
        (int32_t)(((int64_t)(voltage_mv - H2_ESP_TIGA_BATTERY_USABLE_MIN_MV) *
                   (measured_full_voltage_mv -
                    H2_ESP_TIGA_BATTERY_USABLE_MIN_MV)) /
                  (H2_ESP_TIGA_BATTERY_DISCHARGING_FULL_MV -
                   H2_ESP_TIGA_BATTERY_USABLE_MIN_MV));
  }
  return usable_percent_x100(
      interpolate_curve(measured_curve_voltage_mv, false));
}
