#pragma once
#include "h2/pal/os/h2_pal_task.h"
#include <stdint.h>
#define CONFIG_EFUSE 1
#define CONFIG_CACHE_ENABLE 0
#define CONFIG_FLASH_CP_AP_DIRECT_ACCESS 0
#define BK_OK 0
#define BEKEN_WAIT_FOREVER UINT32_MAX
#define MB_IPC_SEND_CMD 2
#define WAIT_FOR_ANY_EVENT 0
typedef unsigned beken_semaphore_t;
typedef struct {
  unsigned unused;
} efuse_hw_t;
extern efuse_hw_t fake_efuse;
#define EFUSE_LL_REG_BASE(i) ((uintptr_t)&fake_efuse)
int bk_efuse_driver_init(void);
uint32_t rtos_disable_int(void);
void rtos_enable_int(uint32_t);
uint64_t bk_aon_rtc_get_us(void);
int efuse_ll_is_operate_finished(efuse_hw_t*);
void efuse_ll_set_direction_read(efuse_hw_t*);
void efuse_ll_set_addr(efuse_hw_t*, uint8_t);
void efuse_ll_enable(efuse_hw_t*);
void efuse_ll_disable(efuse_hw_t*);
int efuse_ll_is_rd_data_valid(efuse_hw_t*);
uint8_t efuse_ll_get_rd_data(efuse_hw_t*);
int bk_flash_read_bytes(uint32_t, uint8_t*, uint32_t);
void h2_bk_test_cp_vector_copy(uintptr_t, uint32_t*);
int rtos_set_semaphore(beken_semaphore_t*);
int rtos_get_semaphore(beken_semaphore_t*, uint32_t);
int rtos_init_semaphore(beken_semaphore_t*, int);
int rtos_deinit_semaphore(beken_semaphore_t*);
uint32_t mb_ipc_socket(uint8_t, void*);
uint32_t mb_ipc_server_get_connect_handle(uint32_t, unsigned);
int mb_ipc_get_recv_event(uint32_t, unsigned int*);
int mb_ipc_get_recv_data_len(uint32_t);
int mb_ipc_recv(uint32_t, uint8_t*, uint8_t*, uint32_t, uint32_t);
int mb_ipc_send(uint32_t, uint8_t, uint8_t*, uint32_t, uint32_t);
int mb_ipc_server_close(uint32_t, uint32_t);
const h2_pal_task_api_t* h2_bk_platform_task_api(void);
unsigned h2_bk_flash_test_read_control(void);
