#pragma once
#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>
#define BK_OK 0
#define CONFIG_FLASH_CP_AP_DIRECT_ACCESS 0
#define CONFIG_CACHE_ENABLE 0
#define BK_PARTITION_APPLICATION 1u
#define BK_PARTITION_S_APP 3u
typedef struct {
  uint32_t partition_start_addr;
} bk_logic_partition_t;
const bk_logic_partition_t* bk_flash_partition_get_info(uint32_t);
int bk_flash_read_bytes(uint32_t, uint8_t*, uint32_t);
uint8_t bk_ota_get_current_partition(void);
uint32_t rtos_enter_critical(void);
void rtos_exit_critical(uint32_t);
void h2_bk_test_mapped_copy(uintptr_t, uint32_t, void*, size_t);
void SCB_InvalidateDCache_by_Addr(void*, int32_t);
void __DSB(void);
uint32_t mb_ipc_socket(uint8_t, void*);
int mb_ipc_connect(uint32_t, uint8_t, uint8_t, uint32_t);
int mb_ipc_send(uint32_t, uint8_t, uint8_t*, uint32_t, uint32_t);
int mb_ipc_recv(uint32_t, uint8_t*, uint8_t*, uint32_t, uint32_t);
