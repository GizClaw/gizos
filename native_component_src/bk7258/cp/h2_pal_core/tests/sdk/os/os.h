#ifndef TEST_OS_OS_H
#define TEST_OS_OS_H
#include <stddef.h>
#include <stdint.h>
typedef void *beken_thread_t;
typedef void *beken_semaphore_t;
typedef void (*beken_thread_function_t)(void *);
#define kNoErr 0
#define BEKEN_WAIT_FOREVER 0xffffffffu
int rtos_init_semaphore(beken_semaphore_t *, int);
int rtos_set_semaphore(beken_semaphore_t *);
int rtos_get_semaphore(beken_semaphore_t *, uint32_t);
void rtos_deinit_semaphore(beken_semaphore_t *);
int rtos_create_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_create_psram_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
void rtos_delete_thread(beken_thread_t *);
#endif
