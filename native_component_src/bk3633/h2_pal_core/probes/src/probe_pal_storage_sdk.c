#include <stdint.h>

#include "flash.h"
#include "nvds.h"

_Static_assert(FLASH_ERASE_SECTOR_SIZE == 4096u,
               "BK3633 persistent layout requires 4 KiB sectors");
_Static_assert(FLASH_ENV_NVDS_DEF_ADDR_4M_ABS ==
                   (512u * 1024u) - FLASH_ERASE_SECTOR_SIZE,
               "4 Mbit Flash NVDS must occupy the final sector");
_Static_assert(FLASH_ENV_NVDS_DEF_ADDR_8M_ABS ==
                   (1024u * 1024u) - FLASH_ERASE_SECTOR_SIZE,
               "8 Mbit Flash NVDS must occupy the final sector");
_Static_assert(FLASH_ENV_BDADDR_DEF_ADDR_4M_ABS ==
                   FLASH_ENV_NVDS_DEF_ADDR_4M_ABS - FLASH_ERASE_SECTOR_SIZE,
               "4 Mbit Flash BDADDR sector must precede NVDS");
_Static_assert(FLASH_ENV_BDADDR_DEF_ADDR_8M_ABS ==
                   FLASH_ENV_NVDS_DEF_ADDR_8M_ABS - FLASH_ERASE_SECTOR_SIZE,
               "8 Mbit Flash BDADDR sector must precede NVDS");

int main(void)
{
    nvds_tag_len_t len = 1u;
    uint8_t data = 0u;
    uint32_t nvds_address = flash_env.nvds_def_addr_abs;
    uint8_t (*get_fn)(uint8_t, nvds_tag_len_t *, uint8_t *) = nvds_get;
    uint8_t (*put_fn)(uint8_t, nvds_tag_len_t, uint8_t *) = nvds_put;
    uint8_t (*del_fn)(uint8_t) = nvds_del;
    uint8_t (*read_fn)(uint8_t, uint32_t, uint32_t, uint8_t *, void (*)(void)) = flash_read;
    uint8_t (*write_fn)(uint8_t, uint32_t, uint32_t, uint8_t *, void (*)(void)) = flash_write;
    uint8_t (*erase_fn)(uint8_t, uint32_t, uint32_t, void (*)(void)) = flash_erase;
    return (get_fn == 0 || put_fn == 0 || del_fn == 0 || read_fn == 0 ||
            write_fn == 0 || erase_fn == 0 || nvds_address == 0u || len != 1u ||
            data != 0u) ? 1 : 0;
}
