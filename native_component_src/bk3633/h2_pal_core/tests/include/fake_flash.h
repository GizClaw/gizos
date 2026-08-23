#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include "h2_bk3633_pal_storage_internal.h"

#include <stddef.h>
#include <stdint.h>

#define FAKE_FLASH_CAPACITY (64u * 1024u)

typedef struct fake_flash {
    uint8_t bytes[FAKE_FLASH_CAPACITY];
    h2_pal_result_t next_read_result;
    h2_pal_result_t next_erase_result;
    h2_pal_result_t next_write_result;
} fake_flash_t;

void fake_flash_init(fake_flash_t *fake);
const h2_bk3633_flash_driver_t *fake_flash_driver(fake_flash_t *fake);

#endif
