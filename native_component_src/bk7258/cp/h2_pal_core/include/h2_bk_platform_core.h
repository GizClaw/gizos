#ifndef H2_BK_PLATFORM_CORE_H
#define H2_BK_PLATFORM_CORE_H

#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_bk_task_stack_region {
  H2_BK_TASK_STACK_DEFAULT = 0,
  H2_BK_TASK_STACK_PSRAM = 1,
} h2_bk_task_stack_region_t;

typedef struct h2_bk_task_policy {
  const char *sdk_name;
  uint32_t priority;
  uint32_t min_stack_size;
  h2_bk_task_stack_region_t stack_region;
} h2_bk_task_policy_t;

typedef h2_pal_result_t (*h2_bk_task_policy_resolver_t)(
    void *user, const char *name, h2_bk_task_policy_t *out_policy);

typedef struct h2_bk_task_policy_config {
  h2_bk_task_policy_resolver_t resolver;
  void *resolver_user;
} h2_bk_task_policy_config_t;

h2_pal_mem_api_t *h2_bk_platform_default_allocator(void);
h2_pal_mem_api_t *h2_bk_platform_sram_allocator(void);
h2_pal_mem_api_t *h2_bk_platform_psram_allocator(void);
const h2_pal_log_api_t *h2_bk_platform_log_api(void);
const h2_pal_sync_api_t *h2_bk_platform_sync_api(void);
const h2_pal_task_api_t *h2_bk_platform_task_api(void);
h2_pal_result_t
h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *config);
const h2_pal_queue_api_t *h2_bk_platform_queue_api(void);
const h2_pal_time_api_t *h2_bk_platform_time_api(void);

#ifdef __cplusplus
}
#endif

#endif
