#ifndef H2_BK7258_BOARD_TASK_TEST_SDK_H
#define H2_BK7258_BOARD_TASK_TEST_SDK_H

#include <stddef.h>
#include <stdint.h>

#define BEKEN_DEFAULT_WORKER_PRIORITY 6
#define kNoErr 0

typedef void *beken_thread_t;
typedef uintptr_t beken_thread_arg_t;
typedef void (*beken_thread_function_t)(beken_thread_arg_t user);

void *os_malloc(size_t size);
void os_free(void *ptr);
int rtos_create_thread(
    beken_thread_t *out_thread,
    int priority,
    const char *name,
    beken_thread_function_t entry,
    uint32_t stack_size,
    beken_thread_arg_t user);
void rtos_delete_thread(beken_thread_t *thread);

#endif
