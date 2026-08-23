#include "h2_bk_h2loader.h"

#include "h2_bk_platform_core.h"

#include <stdbool.h>
#include <string.h>

static bool resolve_command_task(void *user, const char *name,
                                 h2_bk_task_policy_t *out_policy) {
  if (user == NULL || name == NULL || out_policy == NULL ||
      (strcmp(name, "h2loader/appcmd") != 0 &&
       strcmp(name, "h2loader/uartcmd") != 0)) {
    return false;
  }
  const uint32_t stack_size = *(const uint32_t *)user;
  *out_policy = (h2_bk_task_policy_t){
      .name = name,
      .core = 0u,
      .priority = 5u,
      .min_stack_size = stack_size,
      .stack_region = H2_BK_TASK_STACK_PSRAM,
  };
  return true;
}

static int configure_command_task_policy(uint32_t *stack_size) {
  h2_pal_mem_api_t *allocator = h2_bk_platform_psram_allocator();
  if (allocator == NULL || stack_size == NULL || *stack_size == 0u) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  return h2_bk_platform_task_configure(resolve_command_task, stack_size,
                                       allocator, false);
}

int h2_bk_h2loader_configure_app_task_policy(void) {
  static uint32_t stack_size = H2_BK_H2LOADER_APP_COMMAND_STACK_SIZE;
  return configure_command_task_policy(&stack_size);
}

int h2_bk_h2loader_configure_loader_task_policy(void) {
  static uint32_t stack_size = H2_BK_H2LOADER_LOADER_COMMAND_STACK_SIZE;
  return configure_command_task_policy(&stack_size);
}
