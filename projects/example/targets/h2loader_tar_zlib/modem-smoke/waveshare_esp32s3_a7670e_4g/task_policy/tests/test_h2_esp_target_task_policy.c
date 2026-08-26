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

static void assert_policy(const char *name, unsigned priority) {
  h2_esp_task_policy_t policy = {0};
  assert(get_policy(name, &policy) == H2_PAL_OK);
  assert(policy.priority == priority);
  assert(policy.core == H2_ESP_TASK_CORE_0);
  assert(policy.min_stack_size == 4096u);
  assert(policy.stack_region == H2_ESP_TASK_STACK_PSRAM);
}

int main(void) {
  h2_esp_task_policy_t policy = {0};
  assert(h2_esp_target_task_policy_install() == H2_PAL_OK);
  assert(s_config.fallback_resolver != NULL);
  assert(s_config.fallback_resolver(s_config.resolver_user, "dynamic-default",
                                    &policy) == H2_PAL_OK);
  assert(policy.priority == 4u);
  assert(policy.core == H2_ESP_TASK_CORE_ANY);
  assert(policy.min_stack_size == 4096u);
  assert(policy.stack_region == H2_ESP_TASK_STACK_PSRAM);
  assert(s_config.fallback_resolver(s_config.resolver_user, "dynamic-default",
                                    NULL) == H2_PAL_ERR_INVALID_ARG);
  assert_policy("h2loader/appcmd", 8u);
  assert_policy("h2loader/return", 8u);
  assert_policy("h2loader/blelink", 6u);
  assert_policy("bleikcp/kcp", 7u);
  assert_policy("bleikcp/server", 5u);
  assert_policy("h2peer/net", 7u);
  assert_policy("h2peer/udp", 7u);
  assert_policy("net/modem_ppp_rx", 7u);
  assert_policy("modem/call_in", 6u);
  assert_policy("modem/gnss_fix", 5u);
  assert(get_policy("bleikcp-speed/kcp", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("bleikcp-speed/server", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("unknown", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("", &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy(NULL, &policy) == H2_PAL_ERR_NOT_FOUND);
  assert(get_policy("unknown", NULL) == H2_PAL_ERR_NOT_FOUND);
  s_configure_result = H2_PAL_ERR_INVALID_STATE;
  assert(h2_esp_target_task_policy_install() == H2_PAL_ERR_INVALID_STATE);
  return 0;
}
