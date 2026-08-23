#ifndef H2_BK3633_NVDS_ABI_TEST_FLASH_H
#define H2_BK3633_NVDS_ABI_TEST_FLASH_H

#include <stdint.h>

typedef struct h2_bk3633_nvds_abi_test_flash_env {
    uint32_t nvds_def_addr_abs;
} h2_bk3633_nvds_abi_test_flash_env_t;

extern h2_bk3633_nvds_abi_test_flash_env_t flash_env;

uint8_t flash_read(uint8_t flash_id, uint32_t address, uint32_t length,
                   uint8_t *data, void *callback);
uint8_t flash_write(uint8_t flash_id, uint32_t address, uint32_t length,
                    uint8_t *data, void *callback);
uint8_t flash_erase(uint8_t flash_id, uint32_t address, uint32_t length,
                    void *callback);

#endif
