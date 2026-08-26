#include "h2_bk_target_task_policy.h"

#include "h2_bk_platform_core.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

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

static int same_string(const char *left, const char *right) {
  if (left == NULL || right == NULL) {
    return left == right;
  }
  return strcmp(left, right) == 0;
}

enum {
  H2_BK_TARGET_TASK_ROUTE_EXACT = 0,
  H2_BK_TARGET_TASK_ROUTE_PREFIX = 1,
};

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

#define H2_BK_ASSERT_TARGET_ROUTE(name_value, mode_value, sdk_name_value,      \
                                  core_value, priority_value,                  \
                                  min_stack_size_value, stack_region_value)    \
  assert(get_policy((name_value), &policy) ==                                  \
         ((mode_value) == H2_BK_TARGET_TASK_ROUTE_PREFIX                       \
              ? H2_PAL_ERR_NOT_FOUND                                           \
              : H2_PAL_OK));                                                   \
  if ((mode_value) == H2_BK_TARGET_TASK_ROUTE_PREFIX) {                        \
    assert(get_policy(name_value "child", &policy) == H2_PAL_OK);              \
  }                                                                            \
  assert(same_string(policy.sdk_name, (sdk_name_value)));                      \
  assert(policy.core == (core_value));                                         \
  assert(policy.priority == (priority_value));                                 \
  assert(policy.min_stack_size == (min_stack_size_value));                     \
  assert(policy.stack_region == (stack_region_value));

  H2_BK_TARGET_TASK_POLICY_ROUTES(H2_BK_ASSERT_TARGET_ROUTE)

#undef H2_BK_ASSERT_TARGET_ROUTE

  assert(get_policy("x", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("unknown", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy(NULL, &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("unknown", NULL) == H2_PAL_ERR_NOT_FOUND);
  s_configure_result = H2_PAL_ERR_INVALID_STATE;
  assert(h2_bk_target_task_policy_install() == H2_PAL_ERR_INVALID_STATE);
  return 0;
}
