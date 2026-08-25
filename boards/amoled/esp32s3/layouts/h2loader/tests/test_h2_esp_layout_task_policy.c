#include "h2_esp_layout_task_policy.h"
#include "h2_esp_platform_core.h"
#include <assert.h>
#include <stddef.h>
static h2_esp_task_policy_config_t captured;
h2_pal_result_t
h2_esp_platform_task_configure(const h2_esp_task_policy_config_t *config) {
  captured = *config;
  return H2_PAL_OK;
}
static h2_pal_result_t get(const char *n, h2_esp_task_policy_t *p) {
  return captured.resolver(captured.resolver_user, n, p);
}
int main(void) {
  h2_esp_task_policy_t p;
  assert(h2_esp_layout_task_policy_install() == H2_PAL_OK);
  assert(captured.unknown_mode == H2_ESP_TASK_UNKNOWN_FALLBACK);
  assert(captured.fallback.priority == 4u &&
         captured.fallback.core == H2_ESP_TASK_CORE_ANY &&
         captured.fallback.min_stack_size == 4096u &&
         captured.fallback.stack_region == H2_ESP_TASK_STACK_PSRAM);
  assert(get("h2loader/appcmd", &p) == H2_PAL_OK && p.priority == 8u &&
         p.core == 0 && p.stack_region == H2_ESP_TASK_STACK_PSRAM);
  assert(get("bleikcp/server", &p) == H2_PAL_OK && p.priority == 5u);
  assert(get("bleikcp-speed/kcp", &p) == H2_PAL_OK);
  assert(get("net/modem_ppp_rx", &p) == H2_PAL_ERR_NOT_FOUND);
  assert(get("unknown", &p) == H2_PAL_ERR_NOT_FOUND);
  assert(get("", &p) == H2_PAL_ERR_NOT_FOUND);
  assert(get(NULL, &p) == H2_PAL_ERR_NOT_FOUND);
  return 0;
}
