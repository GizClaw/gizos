#include "h2_mosaico_peripherals.h"
#include "h2_mosaico_mag.h"
#include "bsp/esp_mosaico.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <math.h>
#include <string.h>

/* IDs are board identities, independent of Runtime component bindings. */
static const h2_pal_periph_info_t peripherals[] = {
    {.id = 101, .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, .name = "ai"},
    {.id = 102, .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, .name = "boot"},
    {.id = 201, .type = H2_PAL_PERIPH_TYPE_IMU, .name = "bmi270"},
    {.id = 202, .type = H2_PAL_PERIPH_TYPE_IMU, .name = "bmm150_0"},
    {.id = 203, .type = H2_PAL_PERIPH_TYPE_IMU, .name = "bmm150_1"},
    {.id = 301, .type = H2_PAL_PERIPH_TYPE_BATTERY, .name = "bq27220"},
    {.id = 401, .type = H2_PAL_PERIPH_TYPE_PWM_SWITCH, .name = "vibration"},
};
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static bool ready;
static bool imu_initialized;
static bool imu_started;
static bool motor_started;
static i2c_master_dev_handle_t battery_device;
static uint16_t motor_duty;

static int result(esp_err_t rc) {
    if (rc == ESP_OK) return H2_PAL_OK;
    if (rc == ESP_ERR_NO_MEM) return H2_PAL_ERR_NO_MEMORY;
    if (rc == ESP_ERR_NOT_SUPPORTED) return H2_PAL_ERR_UNSUPPORTED;
    if (rc == ESP_ERR_TIMEOUT) return H2_PAL_ERR_TIMEOUT;
    return H2_PAL_ERR_IO;
}

int h2_mosaico_peripherals_init(void) {
    if (ready) return H2_PAL_OK;
    if (mutex == NULL) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    if (mutex == NULL) return H2_PAL_ERR_NO_MEMORY;
    const gpio_config_t config = {
        .pin_bit_mask = (UINT64_C(1) << BSP_BUTTON_AI_GPIO) |
                        (UINT64_C(1) << BSP_BUTTON_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const int rc = result(gpio_config(&config));
    if (rc == H2_PAL_OK) ready = true;
    return rc;
}

static int lock(void) {
    if (!ready || mutex == NULL) return H2_PAL_ERR_INVALID_STATE;
    return xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) == pdTRUE
        ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

static int list(void *user, h2_pal_periph_type_t type,
                h2_pal_periph_cb_t callback, void *callback_user) {
    (void)user;
    if (!callback || !h2_pal_periph_type_is_valid_filter(type)) return H2_PAL_ERR_INVALID_ARG;
    for (size_t i = 0; i < sizeof(peripherals) / sizeof(peripherals[0]); ++i) {
        if (type != H2_PAL_PERIPH_TYPE_ANY && peripherals[i].type != type) continue;
        int rc = callback(callback_user, &peripherals[i]);
        if (rc != H2_PAL_OK) return rc;
    }
    return H2_PAL_OK;
}

static int get(void *user, h2_pal_periph_id_t id, h2_pal_periph_info_t *out) {
    (void)user;
    if (!out) return H2_PAL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < sizeof(peripherals) / sizeof(peripherals[0]); ++i) {
        if (peripherals[i].id == id) { *out = peripherals[i]; return H2_PAL_OK; }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int button_read(void *user, h2_pal_periph_id_t id,
                       h2_pal_single_button_reading_t *out) {
    (void)user;
    if (!out) return H2_PAL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (id != 101 && id != 102) return H2_PAL_ERR_NOT_FOUND;
    int rc = lock();
    if (rc != H2_PAL_OK) return rc;
    out->id = id;
    out->state = gpio_get_level(id == 101 ? BSP_BUTTON_AI_GPIO : BSP_BUTTON_BOOT_GPIO)
        == BSP_BUTTON_ACTIVE_LEVEL ? H2_PAL_BUTTON_STATE_PRESSED : H2_PAL_BUTTON_STATE_RELEASED;
    xSemaphoreGive(mutex);
    return H2_PAL_OK;
}

static int imu_read(void *user, h2_pal_periph_id_t id, h2_pal_imu_reading_t *out) {
    (void)user;
    if (!out) return H2_PAL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (id < 201 || id > 203) return H2_PAL_ERR_NOT_FOUND;
    int rc = lock();
    if (rc != H2_PAL_OK) return rc;
    h2_pal_imu_reading_t reading = {.id = id};
    if (id == 201) {
        if (!imu_started) {
            if (!imu_initialized) {
                rc = result(bsp_imu_init());
                if (rc == H2_PAL_OK) imu_initialized = true;
            }
            if (rc == H2_PAL_OK) rc = result(bsp_imu_start(NULL));
            if (rc == H2_PAL_OK) imu_started = true;
        }
        float ax, ay, az, gx, gy, gz;
        if (rc == H2_PAL_OK) rc = result(bsp_imu_get_accel(&ax, &ay, &az));
        if (rc == H2_PAL_OK) rc = result(bsp_imu_get_gyro(&gx, &gy, &gz));
        if (rc == H2_PAL_OK) {
            reading.flags = H2_PAL_IMU_HAS_ACCEL | H2_PAL_IMU_HAS_GYRO;
            reading.accel_mg = (h2_pal_vec3_i32_t){(int32_t)(ax * 1000), (int32_t)(ay * 1000), (int32_t)(az * 1000)};
            reading.gyro_mdps = (h2_pal_vec3_i32_t){(int32_t)(gx * 1000), (int32_t)(gy * 1000), (int32_t)(gz * 1000)};
        }
    } else {
        const bsp_magnetometer_t sensor = id == 202 ? BSP_MAGNETOMETER_0 : BSP_MAGNETOMETER_1;
        struct bmm150_mag_data data = {0};
        rc = result(bsp_magnetometer_init(sensor));
        if (rc == H2_PAL_OK) rc = result(bsp_magnetometer_read(sensor, &data));
        if (rc == H2_PAL_OK) {
            reading.flags = H2_PAL_IMU_HAS_MAG;
            /* Bosch compensated values are microtesla; PAL uses milligauss. */
            reading.mag_mgauss.x = h2_mosaico_mag_axis(data.x, H2_PAL_IMU_MAG_X_SATURATED, &reading.flags);
            reading.mag_mgauss.y = h2_mosaico_mag_axis(data.y, H2_PAL_IMU_MAG_Y_SATURATED, &reading.flags);
            reading.mag_mgauss.z = h2_mosaico_mag_axis(data.z, H2_PAL_IMU_MAG_Z_SATURATED, &reading.flags);
        }
    }
    if (rc == H2_PAL_OK) *out = reading;
    xSemaphoreGive(mutex);
    return rc;
}

/* TI BQ27220 TRM SLUUBD4: read-only standard command words, little endian.
 * Do not call bsp_battery_init(): it rewrites the cell's CEDV profile. */
static int battery_word(uint8_t command, uint16_t *out) {
    uint8_t bytes[2];
    const int rc = result(i2c_master_transmit_receive(battery_device, &command, 1,
                                                    bytes, sizeof(bytes), 100));
    if (rc == H2_PAL_OK) *out = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return rc;
}

static int battery_read(void *user, h2_pal_periph_id_t id, h2_pal_battery_reading_t *out) {
    (void)user;
    if (!out) return H2_PAL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (id != 301) return H2_PAL_ERR_NOT_FOUND;
    int rc = lock();
    if (rc != H2_PAL_OK) return rc;
    if (battery_device == NULL) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BSP_BATTERY_I2C_ADDR,
            .scl_speed_hz = BSP_BATTERY_I2C_SPEED_HZ,
        };
        rc = result(i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &battery_device));
    }
    uint16_t voltage = 0, current = 0, percent = 0;
    if (rc == H2_PAL_OK) rc = battery_word(0x08, &voltage);
    if (rc == H2_PAL_OK) rc = battery_word(0x0c, &current);
    if (rc == H2_PAL_OK) rc = battery_word(0x2c, &percent);
    if (rc == H2_PAL_OK && percent > 100) rc = H2_PAL_ERR_IO;
    if (rc == H2_PAL_OK) {
        *out = (h2_pal_battery_reading_t){
            .id = id,
            .flags = H2_PAL_BATTERY_HAS_VOLTAGE_MV | H2_PAL_BATTERY_HAS_CURRENT_MA |
                     H2_PAL_BATTERY_HAS_PERCENT_X100,
            .voltage_mv = voltage,
            .current_ma = current < 0x8000u ? (int32_t)current : (int32_t)current - 65536,
            .percent_x100 = (uint16_t)(percent * 100u),
        };
    }
    xSemaphoreGive(mutex);
    return rc;
}

static int motor_set(void *user, h2_pal_periph_id_t id, uint16_t duty) {
    (void)user;
    if (id != 401) return H2_PAL_ERR_NOT_FOUND;
    if (duty > 10000) return H2_PAL_ERR_INVALID_ARG;
    int rc = lock();
    if (rc != H2_PAL_OK) return rc;
    rc = result(bsp_motor_init());
    if (rc == H2_PAL_OK) {
        motor_started = true;
        rc = result(bsp_motor_set_strength((uint8_t)(duty / 100)));
    }
    if (rc == H2_PAL_OK) motor_duty = (uint16_t)((duty / 100) * 100);
    xSemaphoreGive(mutex);
    return rc;
}

static int motor_get(void *user, h2_pal_periph_id_t id, uint16_t *out) {
    (void)user;
    if (!out) return H2_PAL_ERR_INVALID_ARG;
    *out = 0;
    if (id != 401) return H2_PAL_ERR_NOT_FOUND;
    int rc = lock();
    if (rc != H2_PAL_OK) return rc;
    *out = motor_duty;
    xSemaphoreGive(mutex);
    return H2_PAL_OK;
}

int h2_mosaico_peripherals_deinit(void) {
    if (!ready) return H2_PAL_OK;
    int rc = lock();
    if (rc != H2_PAL_OK) return rc;
    esp_err_t first = motor_started ? bsp_motor_set(false) : ESP_OK;
    esp_err_t imu = imu_started ? bsp_imu_stop() : ESP_OK;
    if (imu == ESP_OK) imu = bsp_imu_deinit();
    const esp_err_t mag0 = bsp_magnetometer_deinit(BSP_MAGNETOMETER_0);
    const esp_err_t mag1 = bsp_magnetometer_deinit(BSP_MAGNETOMETER_1);
    const esp_err_t battery = battery_device ? i2c_master_bus_rm_device(battery_device) : ESP_OK;
    if (battery == ESP_OK) battery_device = NULL;
    if (first == ESP_OK) first = imu;
    if (first == ESP_OK) first = mag0;
    if (first == ESP_OK) first = mag1;
    if (first == ESP_OK) first = battery;
    if (imu == ESP_OK) { imu_started = false; imu_initialized = false; }
    if (first == ESP_OK) { ready = false; motor_duty = 0; }
    xSemaphoreGive(mutex);
    return result(first);
}

static const h2_pal_periph_vtable_t periph_ops = {.list = list, .get = get};
static const h2_pal_button_vtable_t button_ops = {.read_single_button = button_read};
static const h2_pal_imu_vtable_t imu_ops = {.read_imu = imu_read};
static const h2_pal_input_vtable_t input_ops = {.read_battery = battery_read};
static const h2_pal_pwm_switch_vtable_t motor_ops = {.set_duty = motor_set, .get_duty = motor_get};
static const h2_pal_periph_api_t periph_api = {.vtable = &periph_ops};
static const h2_pal_button_api_t button_api = {.vtable = &button_ops};
static const h2_pal_imu_api_t imu_api = {.vtable = &imu_ops};
static const h2_pal_input_api_t input_api = {.vtable = &input_ops};
static const h2_pal_pwm_switch_api_t motor_api = {.vtable = &motor_ops};
const h2_pal_periph_api_t *h2_mosaico_periph(void) { return &periph_api; }
const h2_pal_button_api_t *h2_mosaico_button(void) { return &button_api; }
const h2_pal_imu_api_t *h2_mosaico_imu(void) { return &imu_api; }
const h2_pal_input_api_t *h2_mosaico_input(void) { return &input_api; }
const h2_pal_pwm_switch_api_t *h2_mosaico_motor(void) { return &motor_api; }
