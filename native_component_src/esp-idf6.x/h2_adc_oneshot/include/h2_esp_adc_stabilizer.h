#ifndef H2_ESP_ADC_STABILIZER_H
#define H2_ESP_ADC_STABILIZER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ESP_ADC_RADIO_BUTTON_CONFIRM_SAMPLES 2u
#define H2_ESP_ADC_BINARY_CONFIRM_SAMPLES 2u

typedef struct h2_esp_adc_binary_stabilizer {
    bool stable_value;
    bool candidate_value;
    uint8_t candidate_count;
    bool initialized;
} h2_esp_adc_binary_stabilizer_t;

/**
 * @brief Resets caller-owned binary stabilization state.
 *
 * @param state State to reset. NULL is accepted as a no-op.
 */
void h2_esp_adc_binary_stabilizer_init(
    h2_esp_adc_binary_stabilizer_t *state);

/**
 * @brief Applies one binary sample and returns the stable value.
 *
 * The first valid sample is published immediately. Each later transition
 * requires two consecutive samples of the opposite value. A sample equal to
 * the stable value cancels any pending transition.
 *
 * @param state Caller-owned state modified on success.
 * @param sample Current binary sample.
 * @param out_stable_value Receives the current stable value on success.
 * @return true on success, or false for a NULL state or output pointer. An
 * invalid call does not modify valid state or output storage.
 */
bool h2_esp_adc_binary_stabilizer_update(
    h2_esp_adc_binary_stabilizer_t *state,
    bool sample,
    bool *out_stable_value);

typedef enum h2_esp_adc_radio_button_sample_kind {
    H2_ESP_ADC_RADIO_BUTTON_SAMPLE_UNKNOWN = 0,
    H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED,
    H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED,
} h2_esp_adc_radio_button_sample_kind_t;

typedef struct h2_esp_adc_radio_button_state {
    uint32_t stable_button_id;
    uint32_t candidate_button_id;
    uint8_t candidate_count;
    uint8_t release_count;
} h2_esp_adc_radio_button_state_t;

/**
 * @brief Resets a radio-button group to the stable released state.
 */
void h2_esp_adc_radio_button_init(h2_esp_adc_radio_button_state_t *state);

/**
 * @brief Applies one decoded ADC sample to a radio-button group.
 *
 * A press and a release each require two consecutive matching samples. Once a
 * child is stable, another child and unknown/intermediate samples cannot take
 * ownership; the group must first receive a stable release.
 *
 * @param button_id Nonzero only for a PRESSED sample.
 * @param out_button_id Receives the stable child id, or zero when released.
 * @return true when all arguments and sample fields are valid.
 */
bool h2_esp_adc_radio_button_update(
    h2_esp_adc_radio_button_state_t *state,
    h2_esp_adc_radio_button_sample_kind_t kind,
    uint32_t button_id,
    uint32_t *out_button_id);

/**
 * @brief Converts one raw ADC count into the stabilizer's value unit.
 *
 * The callback runs synchronously while the OneShot service mutex is held. It
 * must be deterministic, must not block, and must not call the same service.
 */
typedef int32_t (*h2_esp_adc_value_transform_fn)(void *user, int raw);

typedef struct h2_esp_adc_value_stabilizer_config {
    /** Reseed delta in value units; zero selects one filtered sample per read. */
    int32_t jump_threshold;
    /** Leading reseed samples to discard; must be zero when threshold is zero. */
    uint8_t discard_samples;
    /** Reseed sample delay; must be zero when threshold is zero. */
    uint32_t sample_interval_us;
    /** Optional conversion applied to each raw sample; NULL is identity. */
    h2_esp_adc_value_transform_fn transform;
    /** Borrowed context passed to transform; ignored when transform is NULL. */
    void *transform_user;
} h2_esp_adc_value_stabilizer_config_t;

typedef enum h2_esp_adc_value_read_reason {
    H2_ESP_ADC_VALUE_READ_DIRECT = 0,
    H2_ESP_ADC_VALUE_READ_STEADY,
    H2_ESP_ADC_VALUE_READ_STARTUP,
    H2_ESP_ADC_VALUE_READ_JUMP,
} h2_esp_adc_value_read_reason_t;

typedef struct h2_esp_adc_value_reading {
    h2_esp_adc_value_read_reason_t reason;
    int32_t stable_value;
    int32_t immediate_value;
} h2_esp_adc_value_reading_t;

typedef struct h2_esp_adc_percent_stabilizer {
    uint16_t published_percent_x100;
    bool initialized;
    bool charging;
} h2_esp_adc_percent_stabilizer_t;

void h2_esp_adc_percent_stabilizer_init(
    h2_esp_adc_percent_stabilizer_t *state);

/**
 * @brief Applies one-percent hysteresis and charge-direction monotonicity.
 *
 * While discharging, short recovery cannot increase the published value.
 * While charging, short load transients cannot decrease it. A charge-state
 * transition resets the directional baseline to the current candidate.
 */
bool h2_esp_adc_percent_stabilizer_update(
    h2_esp_adc_percent_stabilizer_t *state,
    uint16_t candidate_percent_x100,
    bool charging,
    uint16_t *out_percent_x100);

#ifdef __cplusplus
}
#endif

#endif
