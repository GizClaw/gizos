#include "h2_esp_adc_oneshot.h"
#include "h2_esp_adc_stabilizer_internal.h"

#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_rom_sys.h"

#include <stdlib.h>

typedef enum h2_esp_adc_calibration_scheme {
    H2_ESP_ADC_CALIBRATION_NONE = 0,
    H2_ESP_ADC_CALIBRATION_CURVE_FITTING,
    H2_ESP_ADC_CALIBRATION_LINE_FITTING,
} h2_esp_adc_calibration_scheme_t;

typedef struct h2_esp_adc_channel_state {
    h2_esp_adc_oneshot_channel_config_t config;
    adc_cali_handle_t calibration;
    h2_esp_adc_calibration_scheme_t calibration_scheme;
    h2_esp_adc_value_stabilizer_t stabilizer;
} h2_esp_adc_channel_state_t;

typedef struct h2_esp_adc_value_read_context {
    h2_esp_adc_oneshot_t *service;
    adc_channel_t channel;
    esp_err_t read_error;
} h2_esp_adc_value_read_context_t;

struct h2_esp_adc_oneshot {
    adc_oneshot_unit_handle_t unit;
    SemaphoreHandle_t mutex;
    h2_esp_adc_channel_state_t *channels;
    size_t channel_count;
};

static h2_esp_adc_channel_state_t *find_channel(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel) {
    for (size_t i = 0u; i < service->channel_count; ++i) {
        if (service->channels[i].config.channel == channel) {
            return &service->channels[i];
        }
    }
    return NULL;
}

static bool read_value_sample(void *user, int32_t *out_raw) {
    h2_esp_adc_value_read_context_t *context =
        (h2_esp_adc_value_read_context_t *)user;
    int raw = 0;
    context->read_error = adc_oneshot_read(
        context->service->unit, context->channel, &raw);
    if (context->read_error != ESP_OK) {
        return false;
    }
    *out_raw = raw;
    return true;
}

static void wait_value_sample(void *user, uint32_t interval_us) {
    (void)user;
    esp_rom_delay_us(interval_us);
}

static esp_err_t create_calibration(
    adc_unit_t unit,
    h2_esp_adc_channel_state_t *channel) {
    if (!channel->config.enable_calibration) {
        return ESP_OK;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        const adc_cali_curve_fitting_config_t config = {
            .unit_id = unit,
            .chan = channel->config.channel,
            .atten = channel->config.oneshot.atten,
            .bitwidth = channel->config.oneshot.bitwidth,
        };
        const esp_err_t rc = adc_cali_create_scheme_curve_fitting(
            &config, &channel->calibration);
        if (rc == ESP_OK) {
            channel->calibration_scheme = H2_ESP_ADC_CALIBRATION_CURVE_FITTING;
            return ESP_OK;
        }
        if (rc != ESP_ERR_NOT_SUPPORTED) {
            return rc;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        const adc_cali_line_fitting_config_t config = {
            .unit_id = unit,
            .atten = channel->config.oneshot.atten,
            .bitwidth = channel->config.oneshot.bitwidth,
        };
        const esp_err_t rc = adc_cali_create_scheme_line_fitting(
            &config, &channel->calibration);
        if (rc == ESP_OK) {
            channel->calibration_scheme = H2_ESP_ADC_CALIBRATION_LINE_FITTING;
            return ESP_OK;
        }
        if (rc != ESP_ERR_NOT_SUPPORTED) {
            return rc;
        }
    }
#endif

    return ESP_OK;
}

static void delete_calibration(h2_esp_adc_channel_state_t *channel) {
    if (channel->calibration == NULL) {
        return;
    }
    switch (channel->calibration_scheme) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    case H2_ESP_ADC_CALIBRATION_CURVE_FITTING:
        (void)adc_cali_delete_scheme_curve_fitting(channel->calibration);
        break;
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    case H2_ESP_ADC_CALIBRATION_LINE_FITTING:
        (void)adc_cali_delete_scheme_line_fitting(channel->calibration);
        break;
#endif
    default:
        break;
    }
    channel->calibration = NULL;
    channel->calibration_scheme = H2_ESP_ADC_CALIBRATION_NONE;
}

static void release_service(h2_esp_adc_oneshot_t *service) {
    if (service == NULL) {
        return;
    }
    if (service->channels != NULL) {
        for (size_t i = 0u; i < service->channel_count; ++i) {
            delete_calibration(&service->channels[i]);
        }
    }
    if (service->unit != NULL) {
        (void)adc_oneshot_del_unit(service->unit);
    }
    if (service->mutex != NULL) {
        vSemaphoreDelete(service->mutex);
    }
    free(service->channels);
    free(service);
}

esp_err_t h2_esp_adc_oneshot_init(
    const h2_esp_adc_oneshot_config_t *config,
    h2_esp_adc_oneshot_t **out_service) {
    if (config == NULL || out_service == NULL || *out_service != NULL ||
        config->channels == NULL || config->channel_count == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0u; i < config->channel_count; ++i) {
        for (size_t j = i + 1u; j < config->channel_count; ++j) {
            if (config->channels[i].channel == config->channels[j].channel) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    h2_esp_adc_oneshot_t *service = calloc(1u, sizeof(*service));
    if (service == NULL) {
        return ESP_ERR_NO_MEM;
    }
    service->channels = calloc(config->channel_count, sizeof(*service->channels));
    if (service->channels == NULL) {
        release_service(service);
        return ESP_ERR_NO_MEM;
    }
    service->channel_count = config->channel_count;
    service->mutex = xSemaphoreCreateMutex();
    if (service->mutex == NULL) {
        release_service(service);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t rc = adc_oneshot_new_unit(&config->unit, &service->unit);
    if (rc != ESP_OK) {
        release_service(service);
        return rc;
    }

    for (size_t i = 0u; i < config->channel_count; ++i) {
        h2_esp_adc_channel_state_t *channel = &service->channels[i];
        channel->config = config->channels[i];
        rc = adc_oneshot_config_channel(
            service->unit,
            channel->config.channel,
            &channel->config.oneshot);
        if (rc == ESP_OK) {
            rc = create_calibration(config->unit.unit_id, channel);
        }
        if (rc != ESP_OK) {
            release_service(service);
            return rc;
        }
    }

    *out_service = service;
    return ESP_OK;
}

esp_err_t h2_esp_adc_oneshot_deinit(h2_esp_adc_oneshot_t **service) {
    if (service == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    release_service(*service);
    *service = NULL;
    return ESP_OK;
}

esp_err_t h2_esp_adc_oneshot_read_raw(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel,
    int *out_raw) {
    if (service == NULL || out_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_raw = 0;
    if (xSemaphoreTake(service->mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    h2_esp_adc_channel_state_t *channel_state = find_channel(service, channel);
    esp_err_t rc = channel_state == NULL
                       ? ESP_ERR_NOT_FOUND
                       : adc_oneshot_read(service->unit, channel, out_raw);
    (void)xSemaphoreGive(service->mutex);
    return rc;
}

esp_err_t h2_esp_adc_oneshot_read_mv(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel,
    int *out_mv) {
    if (service == NULL || out_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_mv = 0;
    if (xSemaphoreTake(service->mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    h2_esp_adc_channel_state_t *channel_state = find_channel(service, channel);
    esp_err_t rc = ESP_ERR_NOT_FOUND;
    if (channel_state != NULL) {
        rc = channel_state->calibration == NULL
                 ? ESP_ERR_NOT_SUPPORTED
                 : adc_oneshot_get_calibrated_result(
                       service->unit,
                       channel_state->calibration,
                       channel,
                       out_mv);
    }
    (void)xSemaphoreGive(service->mutex);
    return rc;
}

esp_err_t h2_esp_adc_oneshot_stabilizer_init(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel) {
    if (service == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(service->mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    h2_esp_adc_channel_state_t *channel_state = find_channel(service, channel);
    esp_err_t rc = ESP_ERR_NOT_FOUND;
    if (channel_state != NULL) {
        rc = h2_esp_adc_value_stabilizer_init_internal(
            &channel_state->stabilizer)
                 ? ESP_OK
                 : ESP_ERR_INVALID_ARG;
    }
    (void)xSemaphoreGive(service->mutex);
    return rc;
}

esp_err_t h2_esp_adc_oneshot_stabilizer_deinit(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel) {
    if (service == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(service->mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    h2_esp_adc_channel_state_t *channel_state = find_channel(service, channel);
    const esp_err_t rc = channel_state == NULL ? ESP_ERR_NOT_FOUND : ESP_OK;
    if (channel_state != NULL) {
        h2_esp_adc_value_stabilizer_reset_internal(
            &channel_state->stabilizer);
    }
    (void)xSemaphoreGive(service->mutex);
    return rc;
}

esp_err_t h2_esp_adc_oneshot_read_value(
    h2_esp_adc_oneshot_t *service,
    adc_channel_t channel,
    h2_esp_adc_value_reading_t *out_reading) {
    if (service == NULL || out_reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(service->mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    h2_esp_adc_channel_state_t *channel_state = find_channel(service, channel);
    esp_err_t rc = ESP_ERR_NOT_FOUND;
    h2_esp_adc_value_reading_t reading = {0};
    if (channel_state != NULL && !channel_state->stabilizer.configured) {
        int raw = 0;
        rc = adc_oneshot_read(service->unit, channel, &raw);
        if (rc == ESP_OK) {
            reading.reason = H2_ESP_ADC_VALUE_READ_DIRECT;
            reading.stable_raw = raw;
            reading.immediate_raw = raw;
        }
    } else if (channel_state != NULL) {
        h2_esp_adc_value_read_context_t context = {
            .service = service,
            .channel = channel,
            .read_error = ESP_OK,
        };
        const bool stabilized = h2_esp_adc_value_stabilizer_read_internal(
            &channel_state->stabilizer,
            read_value_sample,
            wait_value_sample,
            &context,
            &reading);
        rc = stabilized
                 ? ESP_OK
                 : (context.read_error == ESP_OK
                        ? ESP_ERR_INVALID_STATE
                        : context.read_error);
    }
    if (rc == ESP_OK) {
        *out_reading = reading;
    }
    (void)xSemaphoreGive(service->mutex);
    return rc;
}
