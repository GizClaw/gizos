#include "h2_esp_target_task_policy.h"

#include "h2_esp_platform_core.h"

#include <assert.h>
#include <stddef.h>

static h2_esp_task_policy_config_t s_config;
static h2_pal_result_t s_configure_result = H2_PAL_OK;

h2_pal_result_t
h2_esp_platform_task_configure(const h2_esp_task_policy_config_t *config) {
  s_config = *config;
  return s_configure_result;
}

static h2_pal_result_t get_policy(const char *name,
                                  h2_esp_task_policy_t *out_policy) {
  return s_config.resolver(s_config.resolver_user, name, out_policy);
}

int main(void) {
  h2_esp_task_policy_t policy = {0};
  assert(h2_esp_target_task_policy_install() == H2_PAL_OK);
  assert(s_config.unknown_mode == H2_ESP_TASK_UNKNOWN_FALLBACK);
  assert(s_config.fallback.priority == 4u);
  assert(s_config.fallback.core == H2_ESP_TASK_CORE_ANY);
  assert(s_config.fallback.min_stack_size == 4096u);
  assert(s_config.fallback.stack_region == H2_ESP_TASK_STACK_PSRAM);

#define H2_ESP_ASSERT_TARGET_ROUTE(name_value, priority_value, core_value,     \
                                   min_stack_size_value, stack_region_value)   \
  assert(get_policy((name_value), &policy) == H2_PAL_OK);                      \
  assert(policy.priority == (priority_value));                                 \
  assert(policy.core == (core_value));                                         \
  assert(policy.min_stack_size == (min_stack_size_value));                     \
  assert(policy.stack_region == (stack_region_value));

  H2_ESP_TARGET_TASK_POLICY_ROUTES(H2_ESP_ASSERT_TARGET_ROUTE)

#undef H2_ESP_ASSERT_TARGET_ROUTE

  assert(get_policy("unknown", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy(NULL, &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("unknown", NULL) == H2_PAL_ERR_NOT_FOUND);
  s_configure_result = H2_PAL_ERR_INVALID_STATE;
  assert(h2_esp_target_task_policy_install() == H2_PAL_ERR_INVALID_STATE);
  return 0;
}
