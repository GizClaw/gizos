#pragma once
#include <stdint.h>
typedef void *beken_mutex_t;
#define kNoErr 0
uint32_t rtos_enter_critical(void);
void rtos_exit_critical(uint32_t state);
int rtos_lock_recursive_mutex(beken_mutex_t *mutex);
int rtos_unlock_recursive_mutex(beken_mutex_t *mutex);
