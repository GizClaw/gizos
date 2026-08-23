#include "h2_esp_board_private.h"
#include "h2_esp_szp_board_internal.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_qmi8658.h"

#define H2_SZP_BOOT_BUTTON_ID 101u
#define H2_SZP_QMI8658_MOTION_ID 301u
#define H2_SZP_SOC_TEMPERATURE_ID 501u
#define H2_SZP_BOOT_BUTTON_GPIO GPIO_NUM_0
#define H2_SZP_QMI8658_I2C_SPEED_HZ 100000u

static temperature_sensor_handle_t s_temperature;
static i2c_master_dev_handle_t s_qmi8658_dev;
static h2_qmi8658_t s_qmi8658;
static int s_boot_button_configured;
static int s_qmi8658_opened;

static int known_periph_id(h2_pal_periph_id_t id) {
    return id == H2_SZP_BOOT_BUTTON_ID ||
           id == H2_SZP_QMI8658_MOTION_ID ||
           id == H2_SZP_SOC_TEMPERATURE_ID;
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

    const h2_pal_periph_info_t infos[] = {
        {
            .id = H2_SZP_BOOT_BUTTON_ID,
            .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
            .name = "boot",
        },
        {
            .id = H2_SZP_QMI8658_MOTION_ID,
            .type = H2_PAL_PERIPH_TYPE_IMU,
            .name = "qmi8658",
        },
        {
            .id = H2_SZP_SOC_TEMPERATURE_ID,
            .type = H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR,
            .name = "soc_temp",
        },
    };
    for (size_t i = 0u; i < sizeof(infos) / sizeof(infos[0]); ++i) {
        h2_pal_result_t rc = periph_emit(type_filter, cb, cb_user, infos[i]);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
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

static h2_pal_result_t esp_to_platform(esp_err_t err) {
    if (err == ESP_OK) {
        return H2_PAL_OK;
    }
    if (err == ESP_ERR_NO_MEM) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t qmi_write_reg(void *user, uint8_t reg, uint8_t value) {
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)user;
    const uint8_t data[2] = { reg, value };
    return esp_to_platform(i2c_master_transmit(dev, data, sizeof(data), pdMS_TO_TICKS(100)));
}

static h2_pal_result_t qmi_read_regs(void *user, uint8_t reg, uint8_t *out, size_t len) {
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)user;
    return esp_to_platform(i2c_master_transmit_receive(dev, &reg, 1u, out, len, pdMS_TO_TICKS(100)));
}

static void qmi_sleep_ms(void *user, uint32_t ms) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static h2_pal_result_t ensure_qmi8658(void) {
    if (s_qmi8658_opened) {
        return H2_PAL_OK;
    }

    i2c_master_bus_handle_t bus = h2_esp_szp_board_i2c_bus();
    if (bus == NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }

    if (s_qmi8658_dev == NULL) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = H2_QMI8658_I2C_ADDR_SA0_LOW,
            .scl_speed_hz = H2_SZP_QMI8658_I2C_SPEED_HZ,
        };
        h2_pal_result_t rc = esp_to_platform(i2c_master_bus_add_device(bus, &dev_cfg, &s_qmi8658_dev));
        if (rc != H2_PAL_OK) {
            s_qmi8658_dev = NULL;
            return rc;
        }
    }

    h2_pal_result_t rc = h2_qmi8658_init(&s_qmi8658, (h2_qmi8658_transport_t){
        .user = s_qmi8658_dev,
        .write_reg = qmi_write_reg,
        .read_regs = qmi_read_regs,
        .sleep_ms = qmi_sleep_ms,
    });
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_qmi8658_open(&s_qmi8658);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    s_qmi8658_opened = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t read_single_button(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t *out_reading) {
    (void)user;
    if (id != H2_SZP_BOOT_BUTTON_ID) {
        if (known_periph_id(id)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (!s_boot_button_configured) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << H2_SZP_BOOT_BUTTON_GPIO,
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

    const int level = gpio_get_level(H2_SZP_BOOT_BUTTON_GPIO);
    *out_reading = (h2_pal_single_button_reading_t){
        .id = id,
        .state = level == 0 ? H2_PAL_BUTTON_STATE_PRESSED : H2_PAL_BUTTON_STATE_RELEASED,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t read_motion(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_motion_reading_t *out_reading) {
    (void)user;
    if (id != H2_SZP_QMI8658_MOTION_ID) {
        if (known_periph_id(id)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        return H2_PAL_ERR_NOT_FOUND;
    }

    h2_pal_result_t rc = ensure_qmi8658();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    h2_qmi8658_sample_t sample = { 0 };
    rc = h2_qmi8658_read_sample(&s_qmi8658, &sample);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    *out_reading = (h2_pal_motion_reading_t){
        .id = id,
        .flags = H2_PAL_MOTION_HAS_ACCEL | H2_PAL_MOTION_HAS_GYRO,
        .accel_mg = {
            .x = sample.accel_mg[0],
            .y = sample.accel_mg[1],
            .z = sample.accel_mg[2],
        },
        .gyro_mdps = {
            .x = sample.gyro_mdps[0],
            .y = sample.gyro_mdps[1],
            .z = sample.gyro_mdps[2],
        },
        .mag_mgauss = { 0 },
    };
    return H2_PAL_OK;
}

static h2_pal_result_t read_imu(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_imu_reading_t *out_reading) {
    (void)user;
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_motion_reading_t motion;
    h2_pal_result_t rc = read_motion(NULL, id, &motion);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_reading = (h2_pal_imu_reading_t){
        .id = motion.id,
        .flags = 0u,
        .accel_mg = motion.accel_mg,
        .gyro_mdps = motion.gyro_mdps,
        .mag_mgauss = motion.mag_mgauss,
    };
    if ((motion.flags & H2_PAL_MOTION_HAS_ACCEL) != 0u) {
        out_reading->flags |= H2_PAL_IMU_HAS_ACCEL;
    }
    if ((motion.flags & H2_PAL_MOTION_HAS_GYRO) != 0u) {
        out_reading->flags |= H2_PAL_IMU_HAS_GYRO;
    }
    if ((motion.flags & H2_PAL_MOTION_HAS_MAG) != 0u) {
        out_reading->flags |= H2_PAL_IMU_HAS_MAG;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t read_temperature(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_temperature_reading_t *out_reading) {
    (void)user;
    if (id != H2_SZP_SOC_TEMPERATURE_ID) {
        if (known_periph_id(id)) {
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

const h2_pal_imu_api_t *h2_esp_board_imu_api(void) {
    static const h2_pal_imu_vtable_t vtable = {
        .read_imu = read_imu,
    };
    static const h2_pal_imu_api_t api = {
        .vtable = &vtable,
    };
    return &api;
}

h2_pal_input_api_t *h2_esp_board_input_api(void) {
    static const h2_pal_input_vtable_t vtable = {
        .read_motion = read_motion,
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
