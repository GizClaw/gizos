#ifndef H2_BK7258_BOARD_TASK_TEST_SDK_H
#define H2_BK7258_BOARD_TASK_TEST_SDK_H

#include "h2/pal/os/h2_pal_task.h"

#include <stdint.h>
#include <stdlib.h>

typedef void* beken_thread_t;
typedef void* beken_thread_arg_t;
typedef void (*beken_thread_function_t)(beken_thread_arg_t);
typedef void (*h2_bk7258_board_entry_task_fn)(void*);

enum { BEKEN_APPLICATION_PRIORITY = 2, kNoErr = 0 };

void* os_malloc(size_t size);
void* psram_malloc(size_t size);
void os_free(void* pointer);
h2_pal_result_t h2_bk7258_board_runtime_deinit(void);
void rtos_delete_thread(beken_thread_t* thread);

#define DECLARE_CREATE(name)                                                \
  int name(beken_thread_t* thread, uint8_t priority, const char* task_name, \
           beken_thread_function_t function, uint32_t stack_size,           \
           beken_thread_arg_t arg)
DECLARE_CREATE(rtos_core0_create_thread);
DECLARE_CREATE(rtos_core1_create_thread);
DECLARE_CREATE(rtos_core0_create_psram_thread);
DECLARE_CREATE(rtos_core1_create_psram_thread);
DECLARE_CREATE(rtos_create_thread);
DECLARE_CREATE(rtos_create_psram_thread);
#undef DECLARE_CREATE

h2_pal_result_t h2_bk7258_board_start_entry_task(
    const char* name, h2_bk7258_board_entry_task_fn entry, void* user);

#endif
