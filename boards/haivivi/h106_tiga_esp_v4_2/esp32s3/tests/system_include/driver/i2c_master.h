#ifndef H2_TEST_SHADOW_DRIVER_I2C_MASTER_H
#define H2_TEST_SHADOW_DRIVER_I2C_MASTER_H
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef struct i2c_master_bus* i2c_master_bus_handle_t;
typedef struct i2c_master_dev* i2c_master_dev_handle_t;
#define I2C_ADDR_BIT_LEN_7 0
typedef struct {
  int dev_addr_length;
  uint16_t device_address;
  uint32_t scl_speed_hz;
} i2c_device_config_t;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t* config,
                                    i2c_master_dev_handle_t* device);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t* data, size_t size, int timeout_ms);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
#endif
