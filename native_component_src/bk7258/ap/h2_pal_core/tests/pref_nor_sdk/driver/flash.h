#ifndef H2_PREF_NOR_FLASH_H
#define H2_PREF_NOR_FLASH_H
#include <stdint.h>
#define BK_OK 0
typedef int bk_err_t;
typedef enum { FLASH_PROTECT_NONE, FLASH_PROTECT_ALL } flash_protect_type_t;
bk_err_t bk_flash_read_bytes(uint32_t,uint8_t *,uint32_t);
bk_err_t bk_flash_write_bytes(uint32_t,const uint8_t *,uint32_t);
bk_err_t bk_flash_erase_sector(uint32_t);
flash_protect_type_t bk_flash_get_protect_type(void);
void bk_flash_set_protect_type(flash_protect_type_t);
#endif
