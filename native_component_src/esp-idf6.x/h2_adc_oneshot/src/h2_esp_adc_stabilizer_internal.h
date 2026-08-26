#ifndef H2_ESP_ADC_STABILIZER_INTERNAL_H
#define H2_ESP_ADC_STABILIZER_INTERNAL_H

#include "h2_esp_adc_stabilizer.h"

#include <stddef.h>

#define H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW 5u
#define H2_ESP_ADC_NUMERIC_EMA_DIVISOR 60
#define H2_ESP_ADC_VALUE_SMOOTH_DELTA_RAW 10
#define H2_ESP_ADC_VALUE_RESEED_DISCARD_SAMPLES 5u
#define H2_ESP_ADC_VALUE_RESEED_INTERVAL_US 5000u

typedef struct h2_esp_adc_numeric_stabilizer {
    int32_t samples[H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW];
    size_t sample_count;
    size_t next_sample;
    int64_t ema_q16;
    bool initialized;
} h2_esp_adc_numeric_stabilizer_t;

void h2_esp_adc_numeric_stabilizer_init_internal(
    h2_esp_adc_numeric_stabilizer_t *state);

bool h2_esp_adc_numeric_stabilizer_update_internal(
    h2_esp_adc_numeric_stabilizer_t *state,
    int32_t sample,
    int32_t *out_stable_value);

typedef bool (*h2_esp_adc_value_sample_fn)(
    void *user,
    int32_t *out_value);

typedef void (*h2_esp_adc_value_wait_fn)(
    void *user,
    uint32_t interval_us);

typedef struct h2_esp_adc_value_stabilizer {
    h2_esp_adc_numeric_stabilizer_t filter;
    int32_t stable_raw;
    bool configured;
    bool initialized;
} h2_esp_adc_value_stabilizer_t;

bool h2_esp_adc_value_stabilizer_init_internal(
    h2_esp_adc_value_stabilizer_t *state);

void h2_esp_adc_value_stabilizer_reset_internal(
    h2_esp_adc_value_stabilizer_t *state);

/**
 * @brief Runs one atomic stabilization transaction over component-owned I/O.
 *
 * The caller must serialize the complete call. State and output are unchanged
 * when validation or any sample read fails.
 */
bool h2_esp_adc_value_stabilizer_read_internal(
    h2_esp_adc_value_stabilizer_t *state,
    h2_esp_adc_value_sample_fn sample,
    h2_esp_adc_value_wait_fn wait,
    void *user,
    h2_esp_adc_value_reading_t *out_reading);

#endif
