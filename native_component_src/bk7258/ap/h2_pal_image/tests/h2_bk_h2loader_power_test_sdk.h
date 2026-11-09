#pragma once
#include "hal/h2_pal_power.h"
#include "layout.h"
#include <stddef.h>
#include <stdint.h>
#define BK_OK 0
#define BK_PARTITION_OTA_FINA_EXECUTIVE 1
#define CONFIG_OTA_POSITION_INDEPENDENT_AB 1
typedef enum { UPDATE_A_PART, UPDATE_B_PART } part_flag;
int bk_ota_update_partition_flag(int);
enum { EXEX_A_PART, EXEC_B_PART };
enum { CONFIRM_EXEC_A = 3, CONFIRM_EXEC_B = 4 };
typedef enum { FLASH_PROTECT_NONE, FLASH_PROTECT_ALL } flash_protect_type_t;
typedef struct {
  uint32_t partition_start_addr;
} bk_logic_partition_t;
const bk_logic_partition_t* bk_flash_partition_get_info(int);
int bk_flash_read_bytes(uint32_t, uint8_t*, size_t);
int bk_flash_write_bytes(uint32_t, const uint8_t*, size_t);
int bk_flash_erase_sector(uint32_t);
int bk_flash_set_protect_type(flash_protect_type_t);
flash_protect_type_t bk_flash_get_protect_type(void);
uint8_t bk_ota_get_current_partition(void);
void bk_wdt_force_reboot(void);
int bk_wdt_feed(void);
void bk_ota_double_check_for_execution(void);
void bk_ota_confirm_update_partition(unsigned);
void h2_bk_platform_sd_fs_release(void);

extern int g_fixed_active, g_fixed_app_failed, g_fixed_relay_failed;
extern uint8_t g_fixed_slot;
static inline int h2_bk_fixed_layout_active(void) { return g_fixed_active; }
static inline uint8_t h2_bk_fixed_current_slot(void) {
  return g_fixed_active ? g_fixed_slot : bk_ota_get_current_partition();
}
static inline const h2_fixed_layout_t* h2_bk_fixed_layout(void) {
  static h2_fixed_layout_t layout;
  layout.app_table = g_fixed_slot;
  return &layout;
}
static inline int h2_bk_fixed_app_failed(void) { return g_fixed_app_failed; }
static inline int h2_bk_fixed_relay_failed(void) {
  return g_fixed_relay_failed;
}
static inline int h2_bk_fixed_next_app(void) { return 0; }
static inline int h2_bk_fixed_select(uint32_t id) {
  (void)id;
  return H2_PAL_ERR_UNSUPPORTED;
}
static inline int h2_bk_fixed_confirm_app(void) { return 0; }
static inline int h2_bk_fixed_confirm_loader(void) { return 0; }
static inline int h2_bk_fixed_image_state(uint32_t id,
                                          h2_pal_power_image_state_t* out) {
  (void)id;
  out->state = H2_PAL_POWER_IMAGE_STATE_VALID;
  out->flags = 0;
  return 0;
}
