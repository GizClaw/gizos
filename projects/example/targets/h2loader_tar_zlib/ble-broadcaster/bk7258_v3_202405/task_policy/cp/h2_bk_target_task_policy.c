#include "h2_bk_target_task_policy.h"

#include "h2_bk_platform_core.h"

#include <stdio.h>

static h2_pal_result_t resolve_policy(void *user, const char *name,
                                      h2_bk_task_policy_t *out_policy) {
  (void)user;
  (void)name;
  (void)out_policy;
  return H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_bk_target_task_policy_install(void) {
  static const h2_bk_task_policy_config_t config = {
      .resolver = resolve_policy,
      .resolver_user = NULL,
      .unknown_mode = H2_BK_TASK_UNKNOWN_FALLBACK,
      .fallback =
          {
              .sdk_name = NULL,
              .priority = 6u,
              .min_stack_size = 4096u,
              .stack_region = H2_BK_TASK_STACK_DEFAULT,
          },
  };
  h2_pal_result_t rc = h2_bk_platform_task_configure(&config);
  if (rc == H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_READY unit=cp "
           "target=projects/example/targets/h2loader_tar_zlib/ble-broadcaster/"
           "bk7258_v3_202405\n");
  } else {
    printf("H2_PAL_TASK_POLICY_FAIL unit=cp "
           "target=projects/example/targets/h2loader_tar_zlib/ble-broadcaster/"
           "bk7258_v3_202405 "
           "stage=configure reason=%d\n",
           rc);
  }
  return rc;
}
