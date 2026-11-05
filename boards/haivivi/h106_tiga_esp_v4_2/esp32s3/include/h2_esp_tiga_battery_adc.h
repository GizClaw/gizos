#ifndef H2_ESP_TIGA_BATTERY_ADC_H
#define H2_ESP_TIGA_BATTERY_ADC_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_tiga_battery_adc_sample {
  int32_t raw;         /**< Unfiltered battery-channel ADC count. */
  int32_t voltage_mv;  /**< Existing product conversion of this raw count. */
  bool external_power; /**< Direct board charge-detect input, not current. */
} h2_esp_tiga_battery_adc_sample_t;

/**
 * @brief Read the production battery ADC channel without its value filter.
 * @param[out] out_sample Caller-owned sample, cleared before acquisition.
 * @return H2_PAL_OK, INVALID_ARG for NULL, or the acquisition failure.
 *
 * Uses the production channel configuration and raw-to-voltage conversion.
 * Blocking task-context operation; no percentage or voltage stabilization is
 * applied. Call from one diagnostic task with Runtime input polling stopped;
 * board input initialization must not run concurrently. Owns no caller memory.
 */
h2_pal_result_t h2_esp_tiga_battery_adc_read(
    h2_esp_tiga_battery_adc_sample_t* out_sample);

#ifdef __cplusplus
}
#endif

#endif
