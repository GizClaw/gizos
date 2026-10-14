#include "bk_private/bk_init.h"
#include "h2_application_host_e2e.h"
#include "h2_bk7258_board.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_e2e_application_host.h"
#include <stdio.h>
static void run_host(void *user) {
  (void)user;
  const h2_application_host_config_t config = {.entry =
                                                   h2_application_host_e2e_run,
                                               .run_ms = 100u,
                                               .stack_size = 8192u,
                                               .control_stack_size = 8192u};
  h2_application_host_result_t result = {0};
  h2_pal_result_t rc =
      h2_pal_e2e_application_host_run(NULL, 0u, &config, &result);
  printf("APPLICATION_HOST_SDK rc=%d retained=%d\n", rc, result.retained);
}
int main(void) {
  if (h2_bk_target_task_policy_install() != H2_PAL_OK)
    return -1;
  bk_init();
  return h2_bk7258_board_start_entry_task("bk/application-host", run_host,
                                          NULL);
}
