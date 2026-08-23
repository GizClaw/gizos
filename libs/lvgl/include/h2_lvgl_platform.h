#ifndef H2_LVGL_PLATFORM_H
#define H2_LVGL_PLATFORM_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lvgl_platform_config {
    const h2_pal_mem_api_t *allocator;
    const h2_pal_task_api_t *task_api;
    const h2_pal_sync_api_t *sync_api;
    const h2_pal_queue_api_t *queue_api;
    const h2_pal_time_api_t *time_api;
} h2_lvgl_platform_config_t;

int h2_lvgl_platform_init(const h2_lvgl_platform_config_t *config);
void h2_lvgl_platform_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
