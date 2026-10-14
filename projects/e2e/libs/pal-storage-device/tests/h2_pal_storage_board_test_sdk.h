#ifndef H2_PAL_STORAGE_BOARD_TEST_SDK_H
#define H2_PAL_STORAGE_BOARD_TEST_SDK_H
#include "device_runner.h"
#include "h2_pal_e2e_application_host.h"
#include <stdint.h>
typedef void *beken_thread_t;
#define kNoErr 0
void bk_init(void);
void rtos_delay_milliseconds(uint32_t ms);
int rtos_core0_create_psram_thread(beken_thread_t *thread, int priority,
                                   const char *name, void (*entry)(void *),
                                   uint32_t stack, void *user);
int h2_bk_target_task_policy_install(void);
int h2_bk7258_board_start_entry_task(const char *name, void (*entry)(void *),
                                     void *user);
int h2_bk_h2loader_stop_app_iostreamikcp(void);
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t *runtime, const char *name, uint32_t capabilities);
int h2_bk_h2loader_confirm_current_app(h2_runtime_t *runtime);
#endif
