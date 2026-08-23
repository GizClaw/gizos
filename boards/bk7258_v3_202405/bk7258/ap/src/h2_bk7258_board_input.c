#include "h2_bk7258_board_private.h"

#include <common/bk_err.h>
#include <driver/adc.h>
#include <driver/gpio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "gpio_driver.h"

#define H2_BK7258_SOC_TEMPERATURE_ID 501u
#define H2_BK7258_ADC_KEY_GPIO GPIO_28
#define H2_BK7258_ADC_KEY_CHAN 4

extern bk_err_t bk_phy_get_current_temperature(float *temperature);

static bool s_adc_initialized;

static const h2_pal_periph_radio_button_payload_t s_adc_button_payload = {
    .group_id = H2_BK7258_ADC_BUTTON_GROUP_ID,
};

static h2_pal_result_t map_bk_rc(bk_err_t rc) {
    if (rc == BK_OK) {
        return H2_PAL_OK;
    }
    if (rc == BK_ERR_NULL_PARAM || rc == BK_ERR_PARAM) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t adc_init(void) {
    if (s_adc_initialized) {
        return H2_PAL_OK;
    }

    (void)gpio_dev_unmap(H2_BK7258_ADC_KEY_GPIO);

    h2_pal_result_t rc = map_bk_rc(bk_adc_init((adc_chan_t)H2_BK7258_ADC_KEY_CHAN));
    if (rc != H2_PAL_OK) {
        return rc;
    }

    adc_config_t config;
    memset(&config, 0, sizeof(config));
    config.chan = (adc_chan_t)H2_BK7258_ADC_KEY_CHAN;
    config.adc_mode = ADC_CONTINUOUS_MODE;
    config.src_clk = ADC_SCLK_XTAL_26M;
    config.clk = 3203125;
    config.saturate_mode = ADC_SATURATE_MODE_3;
    config.steady_ctrl = 7;
    config.adc_filter = 0;

    rc = map_bk_rc(bk_adc_set_config(&config));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = map_bk_rc(bk_adc_enable_bypass_clalibration());
    if (rc != H2_PAL_OK) {
        return rc;
    }
    s_adc_initialized = true;
    return H2_PAL_OK;
}

static h2_pal_result_t adc_read_voltage_mv(uint32_t *voltage_mv) {
    h2_pal_result_t rc = adc_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    if (bk_adc_acquire() != BK_OK) {
        return H2_PAL_ERR_IO;
    }

    rc = H2_PAL_OK;
    uint16_t raw = 0;
    if (bk_adc_start() != BK_OK) {
        rc = H2_PAL_ERR_IO;
        goto out_release;
    }
    if (bk_adc_set_channel((adc_chan_t)H2_BK7258_ADC_KEY_CHAN) != BK_OK) {
        rc = H2_PAL_ERR_IO;
        goto out_stop;
    }
    if (bk_adc_read(&raw, 100) != BK_OK) {
        rc = H2_PAL_ERR_IO;
        goto out_stop;
    }

    *voltage_mv = ((uint32_t)raw * 2400u) / 4096u;

out_stop:
    (void)bk_adc_stop();
out_release:
    (void)bk_adc_release();
    if (rc != H2_PAL_OK) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static bool button_is_pressed(const h2_bk7258_button_adc_range_t *range, uint32_t voltage_mv) {
    return voltage_mv >= range->min_mv && voltage_mv <= range->max_mv;
}

static h2_pal_periph_id_t pressed_radio_button_id(uint32_t voltage_mv) {
    for (size_t i = 0; i < h2_bk7258_button_range_count; i += 1u) {
        const h2_bk7258_button_adc_range_t *range = &h2_bk7258_button_ranges[i];
        if (button_is_pressed(range, voltage_mv)) {
            return (h2_pal_periph_id_t)range->id;
        }
    }
    return 0u;
}

static int known_non_temperature_id(h2_pal_periph_id_t id) {
    if (id == H2_BK7258_ADC_BUTTON_GROUP_ID) {
        return 1;
    }
    for (size_t i = 0; i < h2_bk7258_button_range_count; i += 1u) {
        if ((h2_pal_periph_id_t)h2_bk7258_button_ranges[i].id == id) {
            return 1;
        }
    }
    return 0;
}

static const h2_bk7258_button_adc_range_t *find_button_range(h2_pal_periph_id_t id) {
    for (size_t i = 0; i < h2_bk7258_button_range_count; i += 1u) {
        if ((h2_pal_periph_id_t)h2_bk7258_button_ranges[i].id == id) {
            return &h2_bk7258_button_ranges[i];
        }
    }
    return NULL;
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
            .id = H2_BK7258_ADC_BUTTON_GROUP_ID,
            .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON_GROUP,
            .name = "adc_keys",
        });
    if (rc != H2_PAL_OK) {
        return rc;
    }

    for (size_t i = 0u; i < h2_bk7258_button_range_count; ++i) {
        rc = periph_emit(
            type_filter,
            cb,
            cb_user,
            (h2_pal_periph_info_t){
                .id = (h2_pal_periph_id_t)h2_bk7258_button_ranges[i].id,
                .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
                .name = "adc_button",
                .payload = &s_adc_button_payload,
                .payload_size = sizeof(s_adc_button_payload),
            });
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }

    return periph_emit(
        type_filter,
        cb,
        cb_user,
        (h2_pal_periph_info_t){
            .id = H2_BK7258_SOC_TEMPERATURE_ID,
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
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (find_button_range(id) != NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (id == H2_BK7258_ADC_BUTTON_GROUP_ID ||
        id == H2_BK7258_SOC_TEMPERATURE_ID) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t read_radio_button_group(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t *out_reading) {
    (void)user;
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (id != H2_BK7258_ADC_BUTTON_GROUP_ID) {
        if (find_button_range(id) != NULL || id == H2_BK7258_SOC_TEMPERATURE_ID) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        return H2_PAL_ERR_NOT_FOUND;
    }

    uint32_t voltage_mv = 0;
    h2_pal_result_t rc = adc_read_voltage_mv(&voltage_mv);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    *out_reading = (h2_pal_radio_button_group_reading_t){
        .id = id,
        .pressed_button_id = pressed_radio_button_id(voltage_mv),
    };
    return H2_PAL_OK;
}

static h2_pal_result_t read_temperature(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_temperature_reading_t *out_reading) {
    (void)user;
    if (id != H2_BK7258_SOC_TEMPERATURE_ID) {
        if (known_non_temperature_id(id)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        return H2_PAL_ERR_NOT_FOUND;
    }

    float celsius = 0.0f;
    if (bk_phy_get_current_temperature(&celsius) != BK_OK) {
        return H2_PAL_ERR_IO;
    }

    *out_reading = (h2_pal_temperature_reading_t){
        .id = id,
        .flags = H2_PAL_TEMPERATURE_HAS_MILLI_CELSIUS,
        .milli_celsius = (int32_t)(celsius * 1000.0f),
    };
    return H2_PAL_OK;
}

const h2_pal_button_api_t *h2_bk7258_board_button_api(void) {
    static const h2_pal_button_vtable_t vtable = {
        .read_single_button = read_single_button,
        .read_radio_button_group = read_radio_button_group,
    };
    static const h2_pal_button_api_t api = {
        .vtable = &vtable,
    };
    return &api;
}

h2_pal_input_api_t *h2_bk7258_board_input_api(void) {
    static const h2_pal_input_vtable_t vtable = {
        .read_temperature = read_temperature,
    };
    static h2_pal_input_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}

const h2_pal_periph_api_t *h2_bk7258_board_periph_api(void) {
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
