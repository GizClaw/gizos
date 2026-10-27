#ifndef H2_TEST_MOSAICO_BSP_H
#define H2_TEST_MOSAICO_BSP_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_TIMEOUT 3
#define BSP_BUTTON_AI_GPIO 7
#define BSP_BUTTON_BOOT_GPIO 0
#define BSP_BUTTON_ACTIVE_LEVEL 0
#define GPIO_MODE_INPUT 0
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define BSP_BATTERY_I2C_ADDR 0x55
#define BSP_BATTERY_I2C_SPEED_HZ 100000
#define I2C_ADDR_BIT_LEN_7 0
#define BSP_MAGNETOMETER_0 0
#define BSP_MAGNETOMETER_1 1
typedef int bsp_magnetometer_t;
typedef void* i2c_master_dev_handle_t;
typedef struct {
  uint64_t pin_bit_mask;
  int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
typedef struct {
  int dev_addr_length, device_address, scl_speed_hz;
} i2c_device_config_t;
struct bmm150_mag_data {
  int16_t x, y, z;
};
int gpio_config(const gpio_config_t* config);
int gpio_get_level(int pin);
int bsp_imu_init(void);
int bsp_imu_start(const void* config);
int bsp_imu_stop(void);
int bsp_imu_deinit(void);
int bsp_imu_get_accel(float* x, float* y, float* z);
int bsp_imu_get_gyro(float* x, float* y, float* z);
int bsp_magnetometer_init(bsp_magnetometer_t sensor);
int bsp_magnetometer_read(bsp_magnetometer_t sensor,
                          struct bmm150_mag_data* data);
int bsp_magnetometer_deinit(bsp_magnetometer_t sensor);
int bsp_motor_init(void);
int bsp_motor_set_strength(uint8_t percent);
int bsp_motor_set(bool on);
void* bsp_i2c_get_handle(void);
int i2c_master_bus_add_device(void* bus, const i2c_device_config_t* config,
                              i2c_master_dev_handle_t* out);
int i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
int i2c_master_transmit_receive(i2c_master_dev_handle_t device,
                                const uint8_t* command, size_t command_size,
                                uint8_t* bytes, size_t size, unsigned timeout);
#endif
