#include "h2_bk_layout_task_policy.h"

#include "h2_bk_platform_core.h"

#include <stdio.h>
#include <string.h>

static h2_pal_result_t resolve_policy(void *user, const char *name,
                                      h2_bk_task_policy_t *out_policy) {
  (void)user;
  if (name == NULL || out_policy == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (strcmp(name, "h2loader/appcmd") == 0 ||
      strcmp(name, "h2loader/uartcmd") == 0) {
    *out_policy = (h2_bk_task_policy_t){
        .sdk_name = NULL,
        .core = 0u,
        .priority = 5u,
        .min_stack_size = 8192u,
        .stack_region = H2_BK_TASK_STACK_PSRAM,
    };
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_bk_layout_task_policy_install(void) {
  const h2_bk_task_policy_config_t config = {
      .resolver = resolve_policy,
      .resolver_user = NULL,
      .unknown_mode = H2_BK_TASK_UNKNOWN_FALLBACK,
      .fallback =
          {
              .sdk_name = NULL,
              .core = 0u,
              .priority = 7u,
              .min_stack_size = 4096u,
              .stack_region = H2_BK_TASK_STACK_DEFAULT,
          },
      .task_allocator = h2_bk_platform_psram_allocator(),
  };
  h2_pal_result_t rc = h2_bk_platform_task_configure(&config);
  if (rc == H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_READY unit=ap layout=media\n");
  } else {
    printf("H2_PAL_TASK_POLICY_FAIL unit=ap stage=configure reason=%d\n", rc);
  }
  return rc;
}
