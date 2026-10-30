#pragma once
#include "h2/pal/core/h2_pal_errors.h"
#include "h2_loader_boot.h"
#include <stdint.h>
#include <stdio.h>
#define BK_OK 0
#define BK_FAIL -1
#define os_printf(...) ((void)0)
#define RBL_HEAD_POS 4096u
#define RT_OTA_HASH_FNV_SEED 0x811c9dc5u
#define EXEX_A_PART 0u
#define EXEC_B_PART 1u
#define CONFIRM_EXEC_A 3u
#define CONFIRM_EXEC_B 4u
#define UPDATE_A_PART 0u
#define UPDATE_B_PART 1u
#define BK_PARTITION_APPLICATION 1u
#define BK_PARTITION_APPLICATION1 2u
#define BK_PARTITION_S_APP 3u
#define BK_PARTITION_OTA_FINA_EXECUTIVE 4u
#define CONFIG_HTTP_AB_PARTITION 1
#define CONFIG_OTA_POSITION_INDEPENDENT_AB 1
typedef unsigned part_flag;
typedef enum { FLASH_PROTECT_NONE, FLASH_PROTECT_ALL } flash_protect_type_t;
typedef struct {
  uint32_t partition_start_addr, partition_length;
  const char* partition_description;
} bk_logic_partition_t;
struct ota_rbl_head {
  char magic[4];
  uint32_t algo, timestamp;
  char name[16], version[24], sn[24];
  uint32_t crc32, hash, size_raw, size_package, info_crc32;
};
const bk_logic_partition_t* bk_flash_partition_get_info(unsigned);
int bk_flash_read_bytes(uint32_t, uint8_t*, uint32_t);
int bk_flash_write_bytes(uint32_t, const uint8_t*, uint32_t);
int bk_flash_erase_sector(uint32_t);
int bk_flash_set_protect_type(flash_protect_type_t);
flash_protect_type_t bk_flash_get_protect_type(void);
int bk_wdt_feed(void);
int bk_ota_update_partition_flag(int);
void bk_ota_double_check_for_execution(void);
void bk_ota_confirm_update_partition(unsigned);
int ota_get_rbl_head(const bk_logic_partition_t*, struct ota_rbl_head*,
                     uint32_t);
