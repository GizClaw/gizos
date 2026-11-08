#ifndef H2_NFC_TEST_I2C_H
#define H2_NFC_TEST_I2C_H

#include "common/bk_err.h"
#include "driver/i2c_types.h"

bk_err_t bk_i2c_driver_init(void);
bk_err_t bk_i2c_init(i2c_id_t id, const i2c_config_t* config);
bk_err_t bk_i2c_deinit(i2c_id_t id);
uint8_t bk_i2c_get_busstate(int id);
bk_err_t bk_i2c_master_write(i2c_id_t id, uint32_t address, const uint8_t* data,
                             uint32_t size, uint32_t timeout_ms);
bk_err_t bk_i2c_memory_read(i2c_id_t id, const i2c_mem_param_t* params);

#endif
