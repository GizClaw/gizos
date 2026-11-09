#ifndef H2_NFC_TEST_I2C_TYPES_H
#define H2_NFC_TEST_I2C_TYPES_H

#include <stdint.h>

typedef enum { I2C_ID_0 = 0, I2C_ID_1 = 1 } i2c_id_t;
typedef enum { I2C_ADDR_MODE_7BIT = 0 } i2c_addr_mode_t;
typedef enum { I2C_MEM_ADDR_SIZE_8BIT = 0 } i2c_mem_addr_size_t;
typedef struct {
  uint32_t baud_rate;
  i2c_addr_mode_t addr_mode;
  uint16_t slave_addr;
} i2c_config_t;
typedef struct {
  uint32_t dev_addr;
  uint32_t mem_addr;
  i2c_mem_addr_size_t mem_addr_size;
  uint8_t* data;
  uint32_t data_size;
  uint32_t timeout_ms;
} i2c_mem_param_t;

#endif
