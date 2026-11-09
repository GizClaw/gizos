#ifndef H2_BK_PAL_DISK_TEST_SDK_H
#define H2_BK_PAL_DISK_TEST_SDK_H

#include "h2_bk_uart_io_test_sdk.h"

typedef int beken_thread_t;
void bk_init(void);
int rtos_get_core_id(void);
int rtos_core0_create_thread(beken_thread_t*, int, const char*, void (*)(void*),
                             unsigned, void*);
int h2_bk_target_task_policy_install(void);

#endif
