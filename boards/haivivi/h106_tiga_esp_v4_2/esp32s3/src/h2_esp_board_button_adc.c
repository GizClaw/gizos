#include "h2_esp_board_button_adc.h"

#include "h2_esp_board_config.h"

#include <stddef.h>

typedef struct h2_tiga_esp_adc_button_range {
  h2_pal_periph_id_t id;
  int min_raw;
  int max_raw;
} h2_tiga_esp_adc_button_range_t;

/* UID 1cdbd44dddba: pool charging off/blue/white and battery off/white medians.
 * Each core spans the lowest/highest measured median; battery blue is
 * unmeasured. Each internal cut is floor((lower core max + upper core min) /
 * 2); the lower key owns the cut. Keep the first lower guard and expand the
 * last upper edge to the midpoint toward the existing release interval.
 * Release detection and debounce remain in the input provider. */
static const h2_tiga_esp_adc_button_range_t s_adc_button_ranges[] = {
    {H2_TIGA_ESP_BUTTON_CANCEL_ID, 247, 387},
    {H2_TIGA_ESP_BUTTON_OK_ID, 388, 612},
    {H2_TIGA_ESP_BUTTON_RIGHT_ID, 613, 912},
    {H2_TIGA_ESP_BUTTON_LEFT_ID, 913, 1320},
    {H2_TIGA_ESP_BUTTON_VOL_INC_ID, 1321, 1899},
    {H2_TIGA_ESP_BUTTON_VOL_DEC_ID, 1900, 2707},
    {H2_TIGA_ESP_BUTTON_RECORD_ID, 2708, 3563},
};

h2_pal_periph_id_t h2_esp_tiga_adc_button_pressed_id(int raw) {
  for (size_t i = 0;
       i < sizeof(s_adc_button_ranges) / sizeof(s_adc_button_ranges[0]); ++i) {
    const h2_tiga_esp_adc_button_range_t* range = &s_adc_button_ranges[i];
    if (raw >= range->min_raw && raw <= range->max_raw) {
      return range->id;
    }
  }
  return 0u;
}

int h2_esp_tiga_adc_button_id_known(h2_pal_periph_id_t id) {
  for (size_t i = 0;
       i < sizeof(s_adc_button_ranges) / sizeof(s_adc_button_ranges[0]); ++i) {
    if (s_adc_button_ranges[i].id == id) {
      return 1;
    }
  }
  return 0;
}
