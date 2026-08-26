#include "h2_bk_target_task_policy.h"

#include "h2_bk_platform_core.h"

#include <assert.h>
#include <stddef.h>

static h2_bk_task_policy_config_t s_config;
static h2_pal_mem_api_t s_allocator;
static h2_pal_result_t s_configure_result = H2_PAL_OK;

h2_pal_result_t
h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *config) {
  s_config = *config;
  return s_configure_result;
}

h2_pal_mem_api_t *h2_bk_platform_psram_allocator(void) { return &s_allocator; }

static h2_pal_result_t get_policy(const char *name,
                                  h2_bk_task_policy_t *out_policy) {
  return s_config.resolver(s_config.resolver_user, name, out_policy);
}

int main(void) {
  h2_bk_task_policy_t policy = {0};
  assert(h2_bk_target_task_policy_install() == H2_PAL_OK);
  assert(s_config.unknown_mode == H2_BK_TASK_UNKNOWN_FALLBACK);
  assert(s_config.task_allocator == &s_allocator);
  assert(s_config.fallback.sdk_name == NULL);
  assert(s_config.fallback.core == 0u);
  assert(s_config.fallback.priority == 7u);
  assert(s_config.fallback.min_stack_size == 4096u);
  assert(s_config.fallback.stack_region == H2_BK_TASK_STACK_DEFAULT);
  assert(get_policy("h2loader/appcmd", &policy) == H2_PAL_OK &&
         policy.core == 0u && policy.priority == 5u &&
         policy.min_stack_size == 49152u);
  assert(get_policy("h2loader/uartcmd", &policy) == H2_PAL_OK &&
         policy.core == 0u && policy.priority == 5u &&
         policy.min_stack_size == 49152u);
  assert(get_policy("audio-system-music", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("bleikcp-speed/kcp", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("unknown", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy(NULL, &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("unknown", NULL) == H2_PAL_ERR_NOT_FOUND);
  s_configure_result = H2_PAL_ERR_INVALID_STATE;
  assert(h2_bk_target_task_policy_install() == H2_PAL_ERR_INVALID_STATE);
  return 0;
}
