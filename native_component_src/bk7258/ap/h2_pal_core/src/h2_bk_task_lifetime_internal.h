#ifndef H2_BK_TASK_LIFETIME_INTERNAL_H
#define H2_BK_TASK_LIFETIME_INTERNAL_H
#include <os/os.h>
#include "FreeRTOS.h"
#include "task.h"
extern TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t core);
/* A completed PAL worker parks instead of self-deleting. Remove it from both
 * CPUs before deletion so FreeRTOS reclaims its kernel state synchronously. */
static inline void h2_bk_delete_stopped_task(TaskHandle_t task) {
    vTaskSuspend(task);
    for (;;) {
        int running = 0;
        for (BaseType_t core = 0; core < CONFIG_CPU_CNT; ++core)
            if (xTaskGetCurrentTaskHandleForCore(core) == task) running = 1;
        if (!running) break;
        rtos_delay_milliseconds(1);
    }
    vTaskDelete(task);
}
#endif
