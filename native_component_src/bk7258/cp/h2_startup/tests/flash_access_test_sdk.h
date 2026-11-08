#pragma once
#include <stdint.h>
#define BK_OK 0
uint32_t rtos_disable_int(void);
void rtos_enable_int(uint32_t);
int bk_flash_read_bytes(uint32_t, uint8_t*, uint32_t);
void h2_bk_test_cp_vector_copy(uintptr_t, uint32_t*);
