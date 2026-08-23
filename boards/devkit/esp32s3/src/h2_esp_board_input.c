#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"

#include "driver/gpio.h"
#include "driver/temperature_sensor.h"

#define H2_DEVKIT_BOOT_BUTTON_ID 101u
#define H2_DEVKIT_SOC_TEMPERATURE_ID 501u
#define H2_DEVKIT_BOOT_BUTTON_GPIO GPIO_NUM_0

static temperature_sensor_handle_t s_temperature;
static int s_boot_button_configured;

static const h2_pal_periph_led_strip_payload_t s_led_strip_payload = {
    .data_gpio_pin_id = H2_DEVKIT_LED_STRIP_GPIO,
    .led_count = H2_DEVKIT_LED_STRIP_PIXEL_COUNT,
    .channels_per_led = H2_DEVKIT_LED_STRIP_CHANNELS_PER_PIXEL,
};

static int known_non_button_id(h2_pal_periph_id_t id) {
    return id == H2_DEVKIT_SOC_TEMPERATURE_ID ||
           id == H2_DEVKIT_LED_STRIP_ID;
}

static int known_non_temperature_id(h2_pal_periph_id_t id) {
    return id == H2_DEVKIT_BOOT_BUTTON_ID ||
           id == H2_DEVKIT_LED_STRIP_ID;
}

static h2_pal_result_t periph_emit(
    h2_pal_periph_type_t type_filter,
    h2_pal_periph_cb_t cb,
    void *cb_user,
    h2_pal_periph_info_t info) {
    if (type_filter != H2_PAL_PERIPH_TYPE_ANY && info.type != type_filter) {
        return H2_PAL_OK;
    }
    return cb(cb_user, &info);
}

static h2_pal_result_t periph_list(
    void *user,
    h2_pal_periph_type_t type_filter,
    h2_pal_periph_cb_t cb,
    void *cb_user) {
    (void)user;

    h2_pal_result_t rc = periph_emit(
        type_filter,
        cb,
        cb_user,
        (h2_pal_periph_info_t){
            .id = H2_DEVKIT_BOOT_BUTTON_ID,
            .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
            .name = "boot",
        });
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = periph_emit(
        type_filter,
        cb,
        cb_user,
        (h2_pal_periph_info_t){
            .id = H2_DEVKIT_LED_STRIP_ID,
            .type = H2_PAL_PERIPH_TYPE_LED_STRIP,
            .name = "led_strip",
            .payload = &s_led_strip_payload,
            .payload_size = sizeof(s_led_strip_payload),
        });
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return periph_emit(
        type_filter,
        cb,
        cb_user,
        (h2_pal_periph_info_t){
            .id = H2_DEVKIT_SOC_TEMPERATURE_ID,
            .type = H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR,
            .name = "soc_temp",
        });
}

typedef struct periph_find {
    h2_pal_periph_id_t id;
    h2_pal_periph_info_t info;
} periph_find_t;

static h2_pal_result_t periph_find_cb(void *user, const h2_pal_periph_info_t *info) {
    periph_find_t *find = (periph_find_t *)user;
    if (find == NULL || info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (info->id != find->id) {
        return H2_PAL_OK;
    }
    find->info = *info;
    return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t periph_get(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_periph_info_t *out_info) {
    (void)user;
    periph_find_t find = { .id = id };
    h2_pal_result_t rc = periph_list(NULL, H2_PAL_PERIPH_TYPE_ANY, periph_find_cb, &find);
    if (rc != H2_PAL_ERR_WOULD_BLOCK) {
        return rc == H2_PAL_OK ? H2_PAL_ERR_NOT_FOUND : rc;
    }
    *out_info = find.info;
    return H2_PAL_OK;
}

static h2_pal_result_t read_single_button(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t *out_reading) {
    (void)user;
    if (id != H2_DEVKIT_BOOT_BUTTON_ID) {
        if (known_non_button_id(id)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (!s_boot_button_configured) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << H2_DEVKIT_BOOT_BUTTON_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&config) != ESP_OK) {
            return H2_PAL_ERR_IO;
        }
        s_boot_button_configured = 1;
    }

    const int level = gpio_get_level(H2_DEVKIT_BOOT_BUTTON_GPIO);
    *out_reading = (h2_pal_single_button_reading_t){
        .id = id,
        .state = level == 0 ? H2_PAL_BUTTON_STATE_PRESSED : H2_PAL_BUTTON_STATE_RELEASED,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t read_temperature(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_temperature_reading_t *out_reading) {
    (void)user;
    if (id != H2_DEVKIT_SOC_TEMPERATURE_ID) {
        if (known_non_temperature_id(id)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (s_temperature == NULL) {
        const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&config, &s_temperature) != ESP_OK) {
            return H2_PAL_ERR_UNAVAILABLE;
        }
        if (temperature_sensor_enable(s_temperature) != ESP_OK) {
            (void)temperature_sensor_uninstall(s_temperature);
            s_temperature = NULL;
            return H2_PAL_ERR_IO;
        }
    }

    float celsius = 0.0f;
    if (temperature_sensor_get_celsius(s_temperature, &celsius) != ESP_OK) {
        return H2_PAL_ERR_IO;
    }

    *out_reading = (h2_pal_temperature_reading_t){
        .id = id,
        .flags = H2_PAL_TEMPERATURE_HAS_MILLI_CELSIUS,
        .milli_celsius = (int32_t)(celsius * 1000.0f),
    };
    return H2_PAL_OK;
}

const h2_pal_button_api_t *h2_esp_board_button_api(void) {
    static const h2_pal_button_vtable_t vtable = {
        .read_single_button = read_single_button,
    };
    static const h2_pal_button_api_t api = {
        .vtable = &vtable,
    };
    return &api;
}

h2_pal_input_api_t *h2_esp_board_input_api(void) {
    static const h2_pal_input_vtable_t vtable = {
        .read_temperature = read_temperature,
    };
    static h2_pal_input_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}

const h2_pal_periph_api_t *h2_esp_board_periph_api(void) {
    static const h2_pal_periph_vtable_t vtable = {
        .list = periph_list,
        .get = periph_get,
    };
    static const h2_pal_periph_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
