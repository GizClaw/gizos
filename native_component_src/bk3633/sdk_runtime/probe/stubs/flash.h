#ifndef H2_BK3633_PROBE_FLASH_H
#define H2_BK3633_PROBE_FLASH_H

#include <stdint.h>

typedef struct h2_bk3633_probe_flash_env {
    uint32_t bdaddr_def_addr_abs;
} h2_bk3633_probe_flash_env_t;

extern h2_bk3633_probe_flash_env_t flash_env;

void flash_init(void);
void flash_read_data(uint8_t *data, uint32_t address, uint32_t length);

#endif
