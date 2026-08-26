#ifndef TEST_H2_BK_PLATFORM_CORE_H
#define TEST_H2_BK_PLATFORM_CORE_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stdint.h>

typedef enum {
  H2_BK_TASK_STACK_DEFAULT = 0,
  H2_BK_TASK_STACK_PSRAM = 1,
} h2_bk_task_stack_region_t;

typedef enum {
  H2_BK_TASK_UNKNOWN_FALLBACK = 0,
  H2_BK_TASK_UNKNOWN_REJECT = 1,
} h2_bk_task_unknown_mode_t;

typedef struct h2_bk_task_policy {
  const char *sdk_name;
  uint32_t priority;
  uint32_t min_stack_size;
  h2_bk_task_stack_region_t stack_region;
} h2_bk_task_policy_t;

typedef h2_pal_result_t (*h2_bk_task_policy_resolver_t)(void *, const char *,
                                                        h2_bk_task_policy_t *);

typedef struct h2_bk_task_policy_config {
  h2_bk_task_policy_resolver_t resolver;
  void *resolver_user;
  h2_bk_task_unknown_mode_t unknown_mode;
  h2_bk_task_policy_t fallback;
} h2_bk_task_policy_config_t;

h2_pal_result_t
h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *config);

#endif
