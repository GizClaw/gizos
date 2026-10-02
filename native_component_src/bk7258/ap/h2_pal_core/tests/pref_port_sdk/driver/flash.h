#ifndef H2_TEST_PREF_PORT_FLASH_H
#define H2_TEST_PREF_PORT_FLASH_H
#include <stdint.h>
typedef int bk_err_t;
#define BK_OK 0
typedef enum flash_protect_type {
  FLASH_PROTECT_NONE,
  FLASH_PROTECT_ALL,
} flash_protect_type_t;
bk_err_t bk_flash_read_bytes(uint32_t offset, uint8_t *buffer, uint32_t size);
bk_err_t bk_flash_write_bytes(uint32_t offset, const uint8_t *buffer,
                              uint32_t size);
bk_err_t bk_flash_erase_sector(uint32_t offset);
flash_protect_type_t bk_flash_get_protect_type(void);
void bk_flash_set_protect_type(flash_protect_type_t type);
#endif
