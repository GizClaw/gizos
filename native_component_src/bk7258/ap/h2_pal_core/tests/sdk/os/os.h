#ifndef TEST_OS_OS_H
#define TEST_OS_OS_H
#include <stddef.h>
#include <stdint.h>
typedef void *beken_thread_t;
typedef void *beken_semaphore_t;
typedef void *beken_mutex_t;
typedef void (*beken_thread_function_t)(void *);
#define kNoErr 0
#define kGeneralErr -1
#define BEKEN_WAIT_FOREVER (0xFFFFFFFF)
int rtos_create_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_create_psram_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_core0_create_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_core0_create_psram_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_core1_create_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_core1_create_psram_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
void rtos_delete_thread(beken_thread_t *);
uint32_t rtos_enter_critical(void);
void rtos_exit_critical(uint32_t);
int rtos_lock_mutex(beken_mutex_t *);
int rtos_trylock_mutex(beken_mutex_t *);
int rtos_unlock_mutex(beken_mutex_t *);
int rtos_deinit_mutex(beken_mutex_t *);
int rtos_lock_recursive_mutex(beken_mutex_t *);
int rtos_unlock_recursive_mutex(beken_mutex_t *);
int rtos_deinit_recursive_mutex(beken_mutex_t *);
int rtos_get_semaphore(beken_semaphore_t *, uint32_t);
int rtos_set_semaphore(beken_semaphore_t *);
int rtos_deinit_semaphore(beken_semaphore_t *);
#endif
