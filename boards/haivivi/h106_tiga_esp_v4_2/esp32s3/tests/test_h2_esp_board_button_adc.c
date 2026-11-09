#include "h2_esp_board_button_adc.h"

#include "h2_esp_board_config.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>

typedef struct button_expectation {
  h2_pal_periph_id_t id;
  int min_raw;
  int core_min_raw;
  int core_max_raw;
  int max_raw;
} button_expectation_t;

/* UID 1cdbd44dddba, diagnostic 0.1.2: 35 measured conditions, 256 samples
 * each, zero errors. Pool charging three-light and battery two-light cores. */
static const button_expectation_t s_expected_buttons[] = {
    {H2_TIGA_ESP_BUTTON_CANCEL_ID, 247, 275, 331, 387},
    {H2_TIGA_ESP_BUTTON_OK_ID, 388, 443, 511, 612},
    {H2_TIGA_ESP_BUTTON_RIGHT_ID, 613, 713, 803, 912},
    {H2_TIGA_ESP_BUTTON_LEFT_ID, 913, 1021, 1141, 1320},
    {H2_TIGA_ESP_BUTTON_VOL_INC_ID, 1321, 1499, 1656, 1899},
    {H2_TIGA_ESP_BUTTON_VOL_DEC_ID, 1900, 2143, 2350, 2707},
    {H2_TIGA_ESP_BUTTON_RECORD_ID, 2708, 3064, 3327, 3563},
};

/* Each key: charging off/blue/white, then battery off/white; P05/median/P95.
 * Battery blue was not measured. Counts are RAW; transition extrema excluded.
 */
static const int s_serial2_samples[][5][3] = {
    {{285, 288, 292},
     {279, 283, 283},
     {271, 275, 277},
     {320, 331, 335},
     {315, 320, 327}},
    {{458, 463, 466},
     {447, 455, 457},
     {439, 443, 443},
     {502, 511, 521},
     {499, 502, 505}},
    {{729, 739, 740},
     {719, 727, 728},
     {703, 713, 715},
     {791, 803, 807},
     {787, 791, 793}},
    {{1049, 1059, 1061},
     {1035, 1044, 1045},
     {1016, 1021, 1023},
     {1126, 1141, 1145},
     {1121, 1125, 1129}},
    {{1527, 1547, 1551},
     {1509, 1522, 1524},
     {1491, 1499, 1501},
     {1637, 1656, 1661},
     {1629, 1635, 1639}},
    {{2183, 2202, 2205},
     {2155, 2171, 2174},
     {2131, 2143, 2147},
     {2324, 2350, 2357},
     {2315, 2319, 2325}},
    {{3110, 3142, 3145},
     {3083, 3104, 3110},
     {3045, 3064, 3067},
     {3289, 3327, 3342},
     {3277, 3287, 3293}},
};

static void expect_pressed_range(int first_raw, int last_raw,
                                 h2_pal_periph_id_t id) {
  for (int raw = first_raw; raw <= last_raw; ++raw) {
    assert(h2_esp_tiga_adc_button_pressed_id(raw) == id);
  }
}

static void test_low_readings_under_led_load(void) {
  /* Home readings previously below the old Right lower bound. */
  expect_pressed_range(722, 729, H2_TIGA_ESP_BUTTON_RIGHT_ID);
  /* White-light Volume Down P05..P95 was below the old lower bound. */
  expect_pressed_range(2167, 2189, H2_TIGA_ESP_BUTTON_VOL_DEC_ID);
}

static void test_recorded_light_envelopes(void) {
  static const struct {
    h2_pal_periph_id_t id;
    int p05_raw;
    int center_raw;
    int p95_raw;
  } samples[] = {
      /* Earlier UID 1cdbd44ddee2, diagnostic 0.1.1; off, blue, white.
       * Back/OK per-light detail was not captured on that device. */
      {H2_TIGA_ESP_BUTTON_RIGHT_ID, 760, 780, 787},
      {H2_TIGA_ESP_BUTTON_RIGHT_ID, 742, 751, 755},
      {H2_TIGA_ESP_BUTTON_RIGHT_ID, 725, 731, 733},
      {H2_TIGA_ESP_BUTTON_LEFT_ID, 1094, 1115, 1122},
      {H2_TIGA_ESP_BUTTON_LEFT_ID, 1064, 1080, 1082},
      {H2_TIGA_ESP_BUTTON_LEFT_ID, 1045, 1053, 1054},
      {H2_TIGA_ESP_BUTTON_VOL_INC_ID, 1591, 1619, 1625},
      {H2_TIGA_ESP_BUTTON_VOL_INC_ID, 1554, 1572, 1575},
      {H2_TIGA_ESP_BUTTON_VOL_INC_ID, 1518, 1531, 1533},
      {H2_TIGA_ESP_BUTTON_VOL_DEC_ID, 2263, 2299, 2303},
      {H2_TIGA_ESP_BUTTON_VOL_DEC_ID, 2211, 2234, 2237},
      {H2_TIGA_ESP_BUTTON_VOL_DEC_ID, 2167, 2187, 2189},
      {H2_TIGA_ESP_BUTTON_RECORD_ID, 3221, 3286, 3295},
      {H2_TIGA_ESP_BUTTON_RECORD_ID, 3165, 3207, 3212},
      {H2_TIGA_ESP_BUTTON_RECORD_ID, 3117, 3144, 3146},
  };

  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    expect_pressed_range(samples[i].p05_raw, samples[i].p95_raw, samples[i].id);
    assert(h2_esp_tiga_adc_button_pressed_id(samples[i].center_raw) ==
           samples[i].id);
  }
}

static void test_recalibrated_cores_and_gap_midpoints(void) {
  const size_t count =
      sizeof(s_expected_buttons) / sizeof(s_expected_buttons[0]);
  const size_t conditions =
      sizeof(s_serial2_samples[0]) / sizeof(s_serial2_samples[0][0]);
  for (size_t key = 0u; key < count; ++key) {
    int core_min = 4095;
    int core_max = 0;
    for (size_t condition = 0u; condition < conditions; ++condition) {
      const int* sample = s_serial2_samples[key][condition];
      expect_pressed_range(sample[0], sample[2], s_expected_buttons[key].id);
      if (sample[1] < core_min) core_min = sample[1];
      if (sample[1] > core_max) core_max = sample[1];
    }
    assert(s_expected_buttons[key].core_min_raw == core_min);
    assert(s_expected_buttons[key].core_max_raw == core_max);
    if (key + 1u == count) continue;
    int next_min = 4095;
    for (size_t condition = 0u; condition < conditions; ++condition)
      if (s_serial2_samples[key + 1u][condition][1] < next_min)
        next_min = s_serial2_samples[key + 1u][condition][1];
    /* For cores [a,b], [c,d], split the gap at (b+c)/2. */
    assert(core_max < next_min);
    const int cut = (core_max + next_min) / 2;
    assert(s_expected_buttons[key].max_raw == cut);
    assert(s_expected_buttons[key + 1u].min_raw == cut + 1);
    assert(h2_esp_tiga_adc_button_pressed_id(cut) ==
           s_expected_buttons[key].id);
    assert(h2_esp_tiga_adc_button_pressed_id(cut + 1) ==
           s_expected_buttons[key + 1u].id);
  }
}

static void test_outer_guards_and_release_midpoint(void) {
  const size_t last =
      sizeof(s_expected_buttons) / sizeof(s_expected_buttons[0]) - 1u;
  const size_t conditions =
      sizeof(s_serial2_samples[0]) / sizeof(s_serial2_samples[0][0]);
  int first_p05 = 4095;
  int last_p95 = 0;
  for (size_t condition = 0u; condition < conditions; ++condition) {
    if (s_serial2_samples[0][condition][0] < first_p05)
      first_p05 = s_serial2_samples[0][condition][0];
    if (s_serial2_samples[last][condition][2] > last_p95)
      last_p95 = s_serial2_samples[last][condition][2];
  }
  assert(s_expected_buttons[0].min_raw <= first_p05 - 24);
  /* The existing release interval starts at 3800. Its lower edge is the
   * final key's right-side neighbor; release detection itself stays fixed. */
  const int release_min = 3800;
  const int cut = (s_expected_buttons[last].core_max_raw + release_min) / 2;
  assert(s_expected_buttons[last].max_raw == cut);
  assert(last_p95 <= cut && cut < release_min);
  assert(h2_esp_tiga_adc_button_pressed_id(cut) ==
         H2_TIGA_ESP_BUTTON_RECORD_ID);
  assert(h2_esp_tiga_adc_button_pressed_id(cut + 1) == 0u);
}

static void test_reported_main_charging_core_readings(void) {
  /* UID 1cdbd44ddee2, separate Main build, charging with the display on.
   * These repeat observations do not redefine the measured core extrema. */
  static const int raw[] = {3368, 3375, 3373, 3383, 3381,
                            3370, 3385, 3387, 3391};
  for (size_t i = 0u; i < sizeof(raw) / sizeof(raw[0]); ++i)
    assert(h2_esp_tiga_adc_button_pressed_id(raw[i]) ==
           H2_TIGA_ESP_BUTTON_RECORD_ID);
}

static void test_operator_confirmed_playback_readings(void) {
  /* Separate Main playback observations from UID 1cdbd44ddee2.
   * These points are not a complete load distribution or PR hardware QA. */
  static const struct {
    int raw;
    h2_pal_periph_id_t id;
  } points[] = {
      {1012, H2_TIGA_ESP_BUTTON_LEFT_ID},
      {1016, H2_TIGA_ESP_BUTTON_LEFT_ID},
      {1018, H2_TIGA_ESP_BUTTON_LEFT_ID},
      {1479, H2_TIGA_ESP_BUTTON_VOL_INC_ID},
      {2111, H2_TIGA_ESP_BUTTON_VOL_DEC_ID},
      {3019, H2_TIGA_ESP_BUTTON_RECORD_ID},
      {3031, H2_TIGA_ESP_BUTTON_RECORD_ID},
      {3047, H2_TIGA_ESP_BUTTON_RECORD_ID},
      {3059, H2_TIGA_ESP_BUTTON_RECORD_ID},
      {3080, H2_TIGA_ESP_BUTTON_RECORD_ID},
      {3082, H2_TIGA_ESP_BUTTON_RECORD_ID},
  };
  for (size_t i = 0u; i < sizeof(points) / sizeof(points[0]); ++i)
    assert(h2_esp_tiga_adc_button_pressed_id(points[i].raw) == points[i].id);
}

static void test_closed_ranges_and_outer_guards(void) {
  int gap_start = 0;
  for (size_t i = 0;
       i < sizeof(s_expected_buttons) / sizeof(s_expected_buttons[0]); ++i) {
    const button_expectation_t* button = &s_expected_buttons[i];
    expect_pressed_range(button->min_raw, button->max_raw, button->id);
    expect_pressed_range(button->core_min_raw, button->core_max_raw,
                         button->id);
    assert(h2_esp_tiga_adc_button_id_known(button->id));
    /* Internal ranges meet at the midpoint; only outer guards are unknown. */
    expect_pressed_range(gap_start, button->min_raw - 1, 0u);
    gap_start = button->max_raw + 1;
  }
  /* Idle/release samples must never select a pressed child. */
  expect_pressed_range(gap_start, 4095, 0u);
}

static void test_invalid_counts_and_ids(void) {
  assert(h2_esp_tiga_adc_button_pressed_id(INT_MIN) == 0u);
  assert(h2_esp_tiga_adc_button_pressed_id(-1) == 0u);
  assert(h2_esp_tiga_adc_button_pressed_id(4096) == 0u);
  assert(h2_esp_tiga_adc_button_pressed_id(INT_MAX) == 0u);
  /* Expanded cores fill the inter-key gaps. High release transitions
   * remain outside the pressed domain. */
  assert(h2_esp_tiga_adc_button_pressed_id(2827) ==
         H2_TIGA_ESP_BUTTON_RECORD_ID);
  assert(h2_esp_tiga_adc_button_pressed_id(3955) == 0u);
  /* Battery Core release-transition max does not set the stable envelope. */
  assert(h2_esp_tiga_adc_button_pressed_id(3601) == 0u);

  assert(!h2_esp_tiga_adc_button_id_known(0u));
  assert(!h2_esp_tiga_adc_button_id_known(H2_TIGA_ESP_ADC_BUTTON_GROUP_ID));
  assert(!h2_esp_tiga_adc_button_id_known(H2_TIGA_ESP_BOOT_BUTTON_ID));
  assert(!h2_esp_tiga_adc_button_id_known(H2_TIGA_ESP_NFC_READER_ID));
  assert(!h2_esp_tiga_adc_button_id_known(UINT32_MAX));
}

int main(void) {
  test_low_readings_under_led_load();
  test_recorded_light_envelopes();
  test_recalibrated_cores_and_gap_midpoints();
  test_outer_guards_and_release_midpoint();
  test_reported_main_charging_core_readings();
  test_operator_confirmed_playback_readings();
  test_closed_ranges_and_outer_guards();
  test_invalid_counts_and_ids();
  return 0;
}
