#include "h2_bk_target_task_policy.h"

#include "h2_bk_platform_core.h"

#include <assert.h>
#include <stddef.h>

static h2_bk_task_policy_config_t s_config;
static h2_pal_result_t s_configure_result = H2_PAL_OK;

h2_pal_result_t
h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *config) {
  s_config = *config;
  return s_configure_result;
}

int main(void) {
  h2_bk_task_policy_t policy = {0};
  assert(h2_bk_target_task_policy_install() == H2_PAL_OK);
  assert(s_config.unknown_mode == H2_BK_TASK_UNKNOWN_FALLBACK);
  assert(s_config.fallback.sdk_name == NULL);
  assert(s_config.fallback.priority == 6u);
  assert(s_config.fallback.min_stack_size == 4096u);
  assert(s_config.fallback.stack_region == H2_BK_TASK_STACK_DEFAULT);
  assert(s_config.resolver(s_config.resolver_user, "unknown", &policy) ==
         H2_PAL_ERR_NOT_FOUND);
  s_configure_result = H2_PAL_ERR_INVALID_STATE;
  assert(h2_bk_target_task_policy_install() == H2_PAL_ERR_INVALID_STATE);
  return 0;
}
