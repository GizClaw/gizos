#pragma once
#include <stddef.h>
#include <stdint.h>
typedef enum { FLASH_PROTECT_NONE, FLASH_PROTECT_ALL } flash_protect_type_t;
flash_protect_type_t bk_flash_get_protect_type(void);
int bk_flash_set_protect_type(flash_protect_type_t);
int bk_flash_read_bytes(uint32_t, void*, uint32_t);
int bk_flash_write_bytes(uint32_t, const void*, uint32_t);
int bk_flash_erase_sector(uint32_t);
void rtos_delay_milliseconds(uint32_t);
