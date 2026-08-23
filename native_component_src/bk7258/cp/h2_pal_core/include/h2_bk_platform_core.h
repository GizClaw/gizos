#ifndef H2_BK_PLATFORM_CORE_H
#define H2_BK_PLATFORM_CORE_H

#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

h2_pal_mem_api_t *h2_bk_platform_default_allocator(void);
h2_pal_mem_api_t *h2_bk_platform_sram_allocator(void);
h2_pal_mem_api_t *h2_bk_platform_psram_allocator(void);
const h2_pal_log_api_t *h2_bk_platform_log_api(void);
const h2_pal_sync_api_t *h2_bk_platform_sync_api(void);
const h2_pal_task_api_t *h2_bk_platform_task_api(void);
const h2_pal_queue_api_t *h2_bk_platform_queue_api(void);
const h2_pal_time_api_t *h2_bk_platform_time_api(void);

#ifdef __cplusplus
}
#endif

#endif
