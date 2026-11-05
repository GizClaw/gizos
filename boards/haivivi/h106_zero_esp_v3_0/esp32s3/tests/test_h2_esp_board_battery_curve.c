#include "h2_esp_board_battery_curve.h"

#include <assert.h>
#include <stddef.h>

typedef struct battery_curve_expectation {
  int32_t voltage_mv;
  uint16_t percent_x100;
} battery_curve_expectation_t;

typedef struct battery_calibration_expectation {
  int raw;
  int32_t input_voltage_mv;
  int32_t expected_voltage_mv;
} battery_calibration_expectation_t;

static void test_raw_to_voltage_calibration(void) {
  static const battery_calibration_expectation_t expectations[] = {
      {2630, 4500, 4498}, {2600, 4450, 4448}, {2575, 4400, 4407},
      {2540, 4350, 4349}, {2510, 4300, 4300}, {2478, 4250, 4247},
      {2450, 4200, 4201}, {2418, 4150, 4148}, {2390, 4100, 4102},
      {2358, 4050, 4049},
  };

  for (size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); ++i) {
    const int32_t voltage_mv =
        h2_esp_zero_esp_battery_voltage_mv_from_raw(expectations[i].raw);
    const int32_t error_mv = voltage_mv - expectations[i].input_voltage_mv;
    assert(voltage_mv == expectations[i].expected_voltage_mv);
    assert(error_mv >= -7 && error_mv <= 7);
  }

  const int32_t voltage_mv = h2_esp_zero_esp_battery_voltage_mv_from_raw(2450);
  assert(voltage_mv == 4201);
  assert(h2_esp_zero_esp_battery_percent_x100(voltage_mv, true) == 10000u);
  assert(h2_esp_zero_esp_battery_percent_x100(voltage_mv, false) == 10000u);
}

static void test_discharging_knots_and_clamps(void) {
  static const battery_curve_expectation_t expectations[] = {
      {3450, 0u},    {3500, 445u},  {3600, 2181u},
      {3700, 5246u}, {3800, 6980u}, {3900, 8327u},
      {4000, 9468u}, {4099, 9991u}, {4100, 10000u},
  };

  assert(h2_esp_zero_esp_battery_percent_x100(2500, false) == 0u);
  assert(h2_esp_zero_esp_battery_percent_x100(H2_ESP_ZERO_BATTERY_USABLE_MIN_MV,
                                              false) == 0u);
  assert(h2_esp_zero_esp_battery_percent_x100(
             H2_ESP_ZERO_BATTERY_USABLE_MIN_MV + 1, false) > 0u);
  for (size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); ++i) {
    assert(h2_esp_zero_esp_battery_percent_x100(expectations[i].voltage_mv,
                                                false) ==
           expectations[i].percent_x100);
  }
  assert(h2_esp_zero_esp_battery_percent_x100(4101, false) == 10000u);
}

static void test_charging_knots_and_clamps(void) {
  static const battery_curve_expectation_t expectations[] = {
      {3450, 0u},    {3558, 0u},    {3741, 497u},  {3906, 4979u},
      {4072, 7500u}, {4159, 8490u}, {4199, 9938u}, {4200, 10000u},
  };

  assert(h2_esp_zero_esp_battery_percent_x100(2500, true) == 0u);
  for (size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); ++i) {
    assert(h2_esp_zero_esp_battery_percent_x100(expectations[i].voltage_mv,
                                                true) ==
           expectations[i].percent_x100);
  }
  assert(h2_esp_zero_esp_battery_percent_x100(4201, true) == 10000u);
}

static void test_interpolation_and_state_selection(void) {
  assert(h2_esp_zero_esp_battery_percent_x100(4000, true) <
         h2_esp_zero_esp_battery_percent_x100(4000, false));
  assert(h2_esp_zero_esp_battery_percent_x100(4199, true) < 10000u);
  assert(h2_esp_zero_esp_battery_percent_x100(4200, true) == 10000u);
}

int main(void) {
  test_raw_to_voltage_calibration();
  test_discharging_knots_and_clamps();
  test_charging_knots_and_clamps();
  test_interpolation_and_state_selection();
  return 0;
}
