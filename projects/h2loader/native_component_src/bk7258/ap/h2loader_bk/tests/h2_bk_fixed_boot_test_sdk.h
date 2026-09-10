#pragma once
#include "h2/pal/core/h2_pal_errors.h"
#include "layout.h"
#include <stdint.h>
#define H2_BK_H2LOADER_PRIMARY_PARTITION_ID 1u
#define H2_BK_H2LOADER_APP_PARTITION_ID 2u
#define BK_OK 0
#define BK_FAIL -1
typedef uint32_t bk_partition_t;
#define BK_PARTITION_APPLICATION 1u
#define BK_PARTITION_APPLICATION1 2u
#define BK_PARTITION_S_APP 3u
#define BK_PARTITION_OTA_FINA_EXECUTIVE 4u
#define BK_PARTITION_H2_BOOT_REQUEST 5u
typedef enum { FLASH_PROTECT_NONE, FLASH_PROTECT_ALL } flash_protect_type_t;
typedef struct {
  uint32_t partition_start_addr;
  uint32_t partition_length;
} bk_logic_partition_t;
const bk_logic_partition_t *bk_flash_partition_get_info(bk_partition_t);
int bk_flash_read_bytes(uint32_t, uint8_t *, uint32_t);
int bk_flash_write_bytes(uint32_t, const uint8_t *, uint32_t);
int bk_flash_erase_sector(uint32_t);
int bk_flash_set_protect_type(flash_protect_type_t);
flash_protect_type_t bk_flash_get_protect_type(void);
uint8_t bk_ota_get_current_partition(void);
const h2_fixed_layout_t *h2_bk_fixed_layout(void);
int h2_bk_fixed_layout_active(void);
uint8_t h2_bk_fixed_current_slot(void);
int h2_bk_fixed_select(uint32_t partition_id);
int h2_bk_fixed_next_app(void);
int h2_bk_fixed_app_failed(void);
int h2_bk_fixed_confirm_app(void);
int h2_bk_fixed_invalidate_app(void);
int h2_bk_fixed_app_window_holds_loader(void);
int h2_bk_fixed_confirm_loader(void);
