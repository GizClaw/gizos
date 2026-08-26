#ifndef H2_ESP_ADC_ONESHOT_H
#define H2_ESP_ADC_ONESHOT_H

#include "h2_esp_adc_stabilizer.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_adc_oneshot h2_esp_adc_oneshot_t;

typedef struct h2_esp_adc_oneshot_channel_config {
    adc_channel_t channel;
    adc_oneshot_chan_cfg_t oneshot;
    bool enable_calibration;
} h2_esp_adc_oneshot_channel_config_t;

typedef struct h2_esp_adc_oneshot_config {
    adc_oneshot_unit_init_cfg_t unit;
    const h2_esp_adc_oneshot_channel_config_t *channels;
    size_t channel_count;
} h2_esp_adc_oneshot_config_t;

/**
 * @brief Creates an explicitly owned ADC OneShot service.
 *
 * The channel configuration is copied during this call. The caller retains
 * ownership of @p config and its channel array. @p out_service must point to
 * NULL. Reads are serialized by the service and must not run from an ISR.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for invalid or duplicate
 * channel configuration, or an ESP-IDF allocation/driver error.
 */
esp_err_t h2_esp_adc_oneshot_init(
    const h2_esp_adc_oneshot_config_t *config,
    h2_esp_adc_oneshot_t **out_service);

/**
 * @brief Deletes an ADC OneShot service and sets the caller's pointer to NULL.
 *
 * Calling this function with a NULL service is successful. The caller must
 * stop concurrent reads before deinitialization.
 */
esp_err_t h2_esp_adc_oneshot_deinit(h2_esp_adc_oneshot_t **service);

/**
 * @brief Synchronously reads one configured ADC channel as a raw count.
 */
esp_err_t h2_esp_adc_oneshot_read_raw(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel,
    int *out_raw);

/**
 * @brief Synchronously reads one configured ADC channel in millivolts.
 *
 * Returns ESP_ERR_NOT_SUPPORTED when calibration was disabled or no supported
 * calibration scheme could be created. Raw counts are never returned as
 * millivolts.
 */
esp_err_t h2_esp_adc_oneshot_read_mv(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel,
    int *out_mv);

/**
 * @brief Installs or resets a value stabilizer on one configured channel.
 *
 * The service owns the resulting state. Reinitializing a channel discards its
 * previous raw filter and jump baseline. This task-context API is serialized
 * with reads and must not run from an ISR. Raw deltas up to and including ten
 * counts are smoothed; larger deltas use the component-owned reseed policy.
 */
esp_err_t h2_esp_adc_oneshot_stabilizer_init(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel);

/**
 * @brief Removes the optional value stabilizer from one configured channel.
 *
 * Deinitializing a configured channel without an installed stabilizer is also
 * successful. This task-context API is serialized with reads and must not run
 * from an ISR.
 */
esp_err_t h2_esp_adc_oneshot_stabilizer_deinit(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel);

/**
 * @brief Reads one channel using its optional service-owned stabilizer.
 *
 * A channel without an installed stabilizer performs one raw read and returns
 * DIRECT with equal stable and immediate raw values. An installed stabilizer
 * runs its complete raw-count transaction, including component-owned delays,
 * while holding the service mutex. The fixed ten-count smoothing boundary is
 * measured from the last successfully published stable raw baseline. On
 * failure, channel state and @p out_reading remain unchanged. This task-context
 * API must not run from an ISR. Board-specific conversion into physical units
 * occurs after this call.
 */
esp_err_t h2_esp_adc_oneshot_read_value(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel,
    h2_esp_adc_value_reading_t *out_reading);

#ifdef __cplusplus
}
#endif

#endif
