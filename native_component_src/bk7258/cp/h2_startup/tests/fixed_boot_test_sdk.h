#pragma once
#include <stdint.h>

#define BK_OK 0
#define BK_PARTITION_APPLICATION 1u
#define BK_PARTITION_APPLICATION1 2u
#define BK_PARTITION_S_APP 3u
#define BK_PARTITION_OTA_FINA_EXECUTIVE 4u
#define BK_PARTITION_H2_BOOT_REQUEST 5u
#define SCB_ICSR_PENDSTCLR_Msk (1u << 25)
#define SCB_ICSR_PENDSVCLR_Msk (1u << 27)

typedef uint32_t bk_partition_t;
typedef enum { FLASH_PROTECT_NONE, FLASH_PROTECT_ALL } flash_protect_type_t;
typedef struct {
  uint32_t partition_start_addr, partition_length;
} bk_logic_partition_t;
typedef struct {
  uint32_t CTRL;
} h2_bk_test_systick_t;
typedef struct {
  uint32_t ICER[8], ICPR[8];
} h2_bk_test_nvic_t;
typedef struct {
  uint32_t ICSR, VTOR;
} h2_bk_test_scb_t;
extern h2_bk_test_systick_t h2_bk_test_systick;
extern h2_bk_test_nvic_t h2_bk_test_nvic;
extern h2_bk_test_scb_t h2_bk_test_scb;
#define SysTick (&h2_bk_test_systick)
#define NVIC (&h2_bk_test_nvic)
#define SCB (&h2_bk_test_scb)

const bk_logic_partition_t* bk_flash_partition_get_info(bk_partition_t);
int bk_flash_read_bytes(uint32_t, uint8_t*, uint32_t);
int bk_flash_write_bytes(uint32_t, const uint8_t*, uint32_t);
int bk_flash_set_protect_type(flash_protect_type_t);
flash_protect_type_t bk_flash_get_protect_type(void);
void __disable_irq(void);
void SCB_DisableDCache(void);
void SCB_DisableICache(void);
void ARM_MPU_Disable(void);
void __DSB(void);
void __ISB(void);
void h2_bk_test_enter_image(uint32_t, uint32_t);
void h2_bk_cp_try_fixed_app(void);
