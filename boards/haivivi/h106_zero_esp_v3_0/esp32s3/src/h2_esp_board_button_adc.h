#ifndef H2_ESP_ZERO_ESP_BOARD_BUTTON_ADC_H
#define H2_ESP_ZERO_ESP_BOARD_BUTTON_ADC_H

#include "h2/pal/hal/h2_pal_periph.h"

/* Stateless press classification in raw ADC counts, not calibrated mV.
 * Zero means no pressed child; release/debounce remain with the provider. */
h2_pal_periph_id_t h2_esp_zero_esp_adc_button_pressed_id(int raw);
int h2_esp_zero_esp_adc_button_id_known(h2_pal_periph_id_t id);

#endif
