#include "h2_bk_layout_task_policy.h"
#include "h2_bk_platform_core.h"
#include <assert.h>
static h2_bk_task_policy_config_t c;
static h2_pal_mem_api_t mem;
h2_pal_result_t
h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *x) {
  c = *x;
  return H2_PAL_OK;
}
h2_pal_mem_api_t *h2_bk_platform_psram_allocator(void) { return &mem; }
static h2_pal_result_t get(const char *n, h2_bk_task_policy_t *p) {
  return c.resolver(c.resolver_user, n, p);
}
int main(void) {
  h2_bk_task_policy_t p;
  assert(h2_bk_layout_task_policy_install() == H2_PAL_OK);
  assert(c.unknown_mode == H2_BK_TASK_UNKNOWN_FALLBACK &&
         c.task_allocator == &mem);
  assert(c.fallback.core == 0u && c.fallback.priority == 7u &&
         c.fallback.min_stack_size == 4096u &&
         c.fallback.stack_region == H2_BK_TASK_STACK_DEFAULT);
  assert(get("h2loader/appcmd", &p) == H2_PAL_OK && p.priority == 5u &&
         p.min_stack_size == 49152u &&
         p.stack_region == H2_BK_TASK_STACK_PSRAM);
  assert(get("h2loader/uartcmd", &p) == H2_PAL_OK);
  assert(get("audio-system-music", &p) == H2_PAL_ERR_NOT_FOUND);
  assert(get("bleikcp-speed/kcp", &p) == H2_PAL_ERR_NOT_FOUND);
  assert(get("bleikcp-speed/", &p) == H2_PAL_ERR_NOT_FOUND);
  assert(get("cmd/h2test", &p) == H2_PAL_ERR_NOT_FOUND);
  return 0;
}
