#ifndef TEST_OS_OS_H
#define TEST_OS_OS_H
#include <stddef.h>
#include <stdint.h>
typedef void *beken_thread_t;
typedef void *beken_semaphore_t;
typedef void (*beken_thread_function_t)(void *);
#define kNoErr 0
#define kGeneralErr -1
int rtos_create_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_create_psram_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_core0_create_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_core0_create_psram_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_core1_create_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
int rtos_core1_create_psram_thread(beken_thread_t *, uint8_t, const char *, beken_thread_function_t, uint32_t, void *);
void rtos_delete_thread(beken_thread_t *);
#endif
