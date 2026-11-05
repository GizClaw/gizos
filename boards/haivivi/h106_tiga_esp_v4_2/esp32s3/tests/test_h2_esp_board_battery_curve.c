#include "h2_esp_board_battery_curve.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

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
        h2_esp_tiga_battery_voltage_mv_from_raw(expectations[i].raw);
    const int32_t error_mv = voltage_mv - expectations[i].input_voltage_mv;
    assert(voltage_mv == expectations[i].expected_voltage_mv);
    assert(error_mv >= -7 && error_mv <= 7);
  }

  const int32_t voltage_mv = h2_esp_tiga_battery_voltage_mv_from_raw(2450);
  assert(voltage_mv == 4201);
  assert(h2_esp_tiga_battery_percent_x100(voltage_mv, true) == 10000u);
  assert(h2_esp_tiga_battery_percent_x100(voltage_mv, false) == 10000u);
}

static void test_discharging_knots_and_clamps(void) {
  static const battery_curve_expectation_t expectations[] = {
      {3450, 0u},    {3500, 445u},  {3600, 2181u},
      {3700, 5246u}, {3800, 6980u}, {3900, 8327u},
      {4000, 9468u}, {4099, 9991u}, {4100, 10000u},
  };

  assert(h2_esp_tiga_battery_percent_x100(2500, false) == 0u);
  assert(h2_esp_tiga_battery_percent_x100(H2_ESP_TIGA_BATTERY_USABLE_MIN_MV,
                                          false) == 0u);
  assert(h2_esp_tiga_battery_percent_x100(H2_ESP_TIGA_BATTERY_USABLE_MIN_MV + 1,
                                          false) > 0u);
  for (size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); ++i) {
    assert(
        h2_esp_tiga_battery_percent_x100(expectations[i].voltage_mv, false) ==
        expectations[i].percent_x100);
  }
  assert(h2_esp_tiga_battery_percent_x100(4101, false) == 10000u);
}

static void test_charging_knots_and_clamps(void) {
  static const battery_curve_expectation_t expectations[] = {
      {3450, 0u},    {3558, 0u},    {3559, 11u},    {3574, 221u},
      {3600, 528u},  {3620, 764u},  {3700, 2308u},  {3749, 4024u},
      {3789, 5110u}, {3794, 5228u}, {3900, 7157u},  {4000, 8529u},
      {4100, 9542u}, {4199, 9994u}, {4200, 10000u},
  };

  assert(h2_esp_tiga_battery_percent_x100(2500, true) == 0u);
  for (size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); ++i) {
    assert(h2_esp_tiga_battery_percent_x100(expectations[i].voltage_mv, true) ==
           expectations[i].percent_x100);
  }
  assert(h2_esp_tiga_battery_percent_x100(4201, true) == 10000u);
}

static void test_interpolation_and_state_selection(void) {
  assert(h2_esp_tiga_battery_percent_x100(4000, true) <
         h2_esp_tiga_battery_percent_x100(4000, false));
  assert(h2_esp_tiga_battery_percent_x100(4199, true) < 10000u);
  assert(h2_esp_tiga_battery_percent_x100(4200, true) == 10000u);
}

static void test_monotonicity_bounds_and_history_independence(void) {
  /* Cold boot while already charging must not require a prior observation. */
  assert(h2_esp_tiga_battery_percent_x100(3794, true) == 5228u);
  for (unsigned state = 0; state < 2; ++state) {
    const bool charging = state != 0;
    assert(h2_esp_tiga_battery_percent_x100(INT32_MIN, charging) == 0u);
    assert(h2_esp_tiga_battery_percent_x100(INT32_MAX, charging) == 10000u);
    uint16_t previous = 0u;
    for (int32_t voltage_mv = 0; voltage_mv <= 5000; ++voltage_mv) {
      const uint16_t percent =
          h2_esp_tiga_battery_percent_x100(voltage_mv, charging);
      assert(percent >= previous && percent <= 10000u);
      previous = percent;
    }
  }
  for (unsigned i = 0; i < 1000; ++i) {
    (void)h2_esp_tiga_battery_percent_x100(4200, true);
    (void)h2_esp_tiga_battery_percent_x100(3450, false);
    assert(h2_esp_tiga_battery_percent_x100(3794, true) == 5228u);
  }
}

/* Minimal anonymous adjacent telemetry pairs; times are relative to each case.
 * These are consistency regressions, not independent true-SOC measurements.
 * Include both a typical case and an unsettled >10pp residual deliberately.
 */
static void test_telemetry_pairs(void) {
  static const struct {
    uint32_t elapsed_ms;
    int32_t charging_mv;
    int32_t discharging_mv;
    uint16_t minimum_gap_x100;
    uint16_t maximum_gap_x100;
  } pairs[] = {
      {17231u, 3580, 3497, 0u, 150u},     {13161u, 3794, 3702, 0u, 150u},
      {9132u, 3812, 3743, 400u, 700u},    {20583u, 3801, 3761, 1000u, 2000u},
      {11037u, 3768, 3636, 1000u, 1100u}, {846u, 4157, 4124, 0u, 300u},
  };
  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
    assert(pairs[i].elapsed_ms <= 30000u);
    const int32_t gap =
        (int32_t)h2_esp_tiga_battery_percent_x100(pairs[i].charging_mv, true) -
        h2_esp_tiga_battery_percent_x100(pairs[i].discharging_mv, false);
    const uint32_t absolute_gap = (uint32_t)(gap < 0 ? -gap : gap);
    assert(absolute_gap >= pairs[i].minimum_gap_x100);
    assert(absolute_gap <= pairs[i].maximum_gap_x100);
  }
}

int main(void) {
  test_monotonicity_bounds_and_history_independence();
  test_raw_to_voltage_calibration();
  test_discharging_knots_and_clamps();
  test_charging_knots_and_clamps();
  test_interpolation_and_state_selection();
  test_telemetry_pairs();
  return 0;
}
