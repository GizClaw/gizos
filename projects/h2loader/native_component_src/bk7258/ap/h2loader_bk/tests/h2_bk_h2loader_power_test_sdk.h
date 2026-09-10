#pragma once
#include "h2/pal/hal/h2_pal_power.h"
#include <stddef.h>
#include <stdint.h>
#define H2_BK_H2LOADER_PRIMARY_PARTITION_ID 1u
#define H2_BK_H2LOADER_APP_PARTITION_ID 2u
#define BK_OK 0
#define BK_PARTITION_OTA_FINA_EXECUTIVE 1
typedef enum { UPDATE_A_PART, UPDATE_B_PART } part_flag;
enum { EXEX_A_PART, EXEC_B_PART };
enum { CONFIRM_EXEC_A = 3, CONFIRM_EXEC_B = 4 };
typedef struct {
  uint32_t partition_start_addr;
} bk_logic_partition_t;
const bk_logic_partition_t *bk_flash_partition_get_info(int);
int bk_flash_read_bytes(uint32_t, uint8_t *, size_t);
uint8_t bk_ota_get_current_partition(void);
void bk_wdt_force_reboot(void);
int h2_bk_h2loader_commit_staged_app_boot(void);
int h2_bk_h2loader_select_confirmed_boot_partition(uint32_t);
const h2_pal_power_api_t *h2_bk_h2loader_power_api(void);

/* Legacy-bank fixtures exercise the existing fallback path. */
static inline int h2_bk_fixed_layout_active(void) { return 0; }
static inline uint8_t h2_bk_fixed_current_slot(void) { return bk_ota_get_current_partition(); }
static inline int h2_bk_fixed_app_failed(void) { return 0; }
static inline int h2_bk_fixed_next_app(void) { return 0; }
static inline int h2_bk_fixed_select(uint32_t id) { (void)id; return H2_PAL_ERR_UNSUPPORTED; }
