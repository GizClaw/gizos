#include "h2_bk_layout_task_policy.h"
#include "h2_bk_platform_core.h"
#include <assert.h>
static h2_bk_task_policy_config_t c;
h2_pal_result_t
h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *x) {
  c = *x;
  return H2_PAL_OK;
}
int main(void) {
  h2_bk_task_policy_t p;
  assert(h2_bk_layout_task_policy_install() == H2_PAL_OK);
  assert(c.unknown_mode == H2_BK_TASK_UNKNOWN_FALLBACK &&
         c.fallback.priority == 6u && c.fallback.min_stack_size == 4096u &&
         c.fallback.stack_region == H2_BK_TASK_STACK_DEFAULT);
  assert(c.resolver(c.resolver_user, "cmd/h2test", &p) == H2_PAL_ERR_NOT_FOUND);
  return 0;
}
