#ifndef H2_SZP_TEST_I2C_MASTER_H
#define H2_SZP_TEST_I2C_MASTER_H
#include <stdint.h>
typedef void* i2c_master_bus_handle_t;
typedef void* i2c_master_dev_handle_t;
typedef int esp_err_t;
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
                           int timeout);
#endif
