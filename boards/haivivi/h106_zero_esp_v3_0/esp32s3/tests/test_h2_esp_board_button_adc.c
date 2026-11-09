#include "h2_esp_board_button_adc.h"

#include "h2_esp_board_config.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>

/* UID 7c4fadbc2cea, diagnostic 0.1.2: 35 measured conditions, 256 samples
 * each, zero errors. Pool charging three-light and battery two-light cores. */
static const struct {
  h2_pal_periph_id_t id;
  int minimum;
  int core_min;
  int core_max;
  int maximum;
} buttons[] = {
    {H2_ZERO_ESP_BUTTON_CANCEL_ID, 223, 251, 317, 365},
    {H2_ZERO_ESP_BUTTON_OK_ID, 366, 414, 495, 585},
    {H2_ZERO_ESP_BUTTON_RIGHT_ID, 586, 675, 783, 878},
    {H2_ZERO_ESP_BUTTON_LEFT_ID, 879, 973, 1117, 1272},
    {H2_ZERO_ESP_BUTTON_VOL_INC_ID, 1273, 1428, 1621, 1830},
    {H2_ZERO_ESP_BUTTON_VOL_DEC_ID, 1831, 2040, 2296, 2630},
    {H2_ZERO_ESP_BUTTON_RECORD_ID, 2631, 2965, 3275, 3537},
};

/* Each key: charging off/blue/white, then battery off/white; P05/median/P95.
 * Battery blue was not measured. Counts are RAW; transition extrema excluded.
 */
static const int samples[][5][3] = {
    {{282, 287, 291},
     {265, 268, 271},
     {250, 251, 253},
     {311, 317, 327},
     {307, 311, 313}},
    {{453, 460, 463},
     {429, 435, 437},
     {411, 414, 415},
     {487, 495, 501},
     {483, 487, 488}},
    {{723, 733, 739},
     {694, 699, 703},
     {671, 675, 677},
     {773, 783, 789},
     {763, 767, 773}},
    {{1042, 1055, 1062},
     {1001, 1011, 1015},
     {969, 973, 975},
     {1103, 1117, 1124},
     {1096, 1099, 1102}},
    {{1499, 1516, 1519},
     {1459, 1468, 1471},
     {1419, 1428, 1430},
     {1599, 1621, 1626},
     {1590, 1593, 1595}},
    {{2139, 2163, 2167},
     {2086, 2100, 2103},
     {2029, 2040, 2041},
     {2269, 2296, 2300},
     {2259, 2263, 2265}},
    {{3079, 3113, 3117},
     {3007, 3032, 3039},
     {2944, 2965, 2967},
     {3241, 3275, 3287},
     {3228, 3236, 3247}},
};

static void expect_range(int low, int high, h2_pal_periph_id_t id) {
  for (int raw = low; raw <= high; ++raw)
    assert(h2_esp_zero_esp_adc_button_pressed_id(raw) == id);
}

static void test_stable_light_samples(void) {
  const size_t conditions = sizeof(samples[0]) / sizeof(samples[0][0]);
  for (size_t key = 0u; key < sizeof(buttons) / sizeof(buttons[0]); ++key)
    for (size_t condition = 0u; condition < conditions; ++condition)
      expect_range(samples[key][condition][0], samples[key][condition][2],
                   buttons[key].id);
}

static void test_core_gap_midpoint_boundaries(void) {
  const size_t conditions = sizeof(samples[0]) / sizeof(samples[0][0]);
  for (size_t key = 0u; key < sizeof(buttons) / sizeof(buttons[0]); ++key) {
    int core_min = 4095;
    int core_max = 0;
    for (size_t condition = 0u; condition < conditions; ++condition) {
      const int median = samples[key][condition][1];
      if (median < core_min) core_min = median;
      if (median > core_max) core_max = median;
    }
    assert(buttons[key].core_min == core_min);
    assert(buttons[key].core_max == core_max);
    if (key + 1u == sizeof(buttons) / sizeof(buttons[0])) continue;
    int next_min = 4095;
    for (size_t condition = 0u; condition < conditions; ++condition)
      if (samples[key + 1u][condition][1] < next_min)
        next_min = samples[key + 1u][condition][1];
    assert(core_max < next_min);
    const int cut = (core_max + next_min) / 2;
    assert(buttons[key].maximum == cut);
    assert(buttons[key + 1u].minimum == cut + 1);
    assert(h2_esp_zero_esp_adc_button_pressed_id(cut) == buttons[key].id);
    assert(h2_esp_zero_esp_adc_button_pressed_id(cut + 1) ==
           buttons[key + 1u].id);
  }
}

static void test_outer_guards_and_release_midpoint(void) {
  const size_t last = sizeof(buttons) / sizeof(buttons[0]) - 1u;
  const size_t conditions = sizeof(samples[0]) / sizeof(samples[0][0]);
  int first_p05 = 4095;
  int last_p95 = 0;
  for (size_t condition = 0u; condition < conditions; ++condition) {
    if (samples[0][condition][0] < first_p05)
      first_p05 = samples[0][condition][0];
    if (samples[last][condition][2] > last_p95)
      last_p95 = samples[last][condition][2];
  }
  assert(buttons[0].minimum <= first_p05 - 24);
  /* Match the last press boundary against the unchanged release interval. */
  const int release_min = 3800;
  const int cut = (buttons[last].core_max + release_min) / 2;
  assert(buttons[last].maximum == cut);
  assert(last_p95 <= cut && cut < release_min);
  assert(h2_esp_zero_esp_adc_button_pressed_id(cut) ==
         H2_ZERO_ESP_BUTTON_RECORD_ID);
  assert(h2_esp_zero_esp_adc_button_pressed_id(cut + 1) == 0u);
}

static void test_closed_ranges_and_outer_guards(void) {
  int gap_start = 0;
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
    expect_range(buttons[i].minimum, buttons[i].maximum, buttons[i].id);
    expect_range(buttons[i].core_min, buttons[i].core_max, buttons[i].id);
    assert(h2_esp_zero_esp_adc_button_id_known(buttons[i].id));
    expect_range(gap_start, buttons[i].minimum - 1, 0u);
    gap_start = buttons[i].maximum + 1;
  }
  expect_range(gap_start, 4095, 0u);
}

int main(void) {
  test_stable_light_samples();
  test_core_gap_midpoint_boundaries();
  test_outer_guards_and_release_midpoint();
  test_closed_ranges_and_outer_guards();
  assert(h2_esp_zero_esp_adc_button_pressed_id(INT_MIN) == 0u);
  assert(h2_esp_zero_esp_adc_button_pressed_id(-1) == 0u);
  assert(h2_esp_zero_esp_adc_button_pressed_id(4096) == 0u);
  assert(h2_esp_zero_esp_adc_button_pressed_id(INT_MAX) == 0u);
  /* The recorded release-transition extreme is not a pressed child. */
  assert(h2_esp_zero_esp_adc_button_pressed_id(3965) == 0u);
  assert(!h2_esp_zero_esp_adc_button_id_known(0u));
  assert(!h2_esp_zero_esp_adc_button_id_known(H2_ZERO_ESP_ADC_BUTTON_GROUP_ID));
  assert(!h2_esp_zero_esp_adc_button_id_known(H2_ZERO_ESP_BOOT_BUTTON_ID));
  assert(!h2_esp_zero_esp_adc_button_id_known(H2_ZERO_ESP_NFC_READER_ID));
  assert(!h2_esp_zero_esp_adc_button_id_known(UINT32_MAX));
  return 0;
}
