#ifndef H2_P4_TEST_I2C_H
#define H2_P4_TEST_I2C_H
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef void* i2c_master_bus_handle_t;
typedef void* i2c_master_dev_handle_t;
enum { I2C_CLK_SRC_DEFAULT };
typedef struct {
  int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
  struct {
    bool enable_internal_pullup;
  } flags;
} i2c_master_bus_config_t;
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* config,
                             i2c_master_bus_handle_t* out);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
                           int timeout);
#endif
