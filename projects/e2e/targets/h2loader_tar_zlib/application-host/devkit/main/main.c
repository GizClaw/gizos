#include "h2_application_host_e2e.h"
#include "h2_esp_target_task_policy.h"
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
void app_main(void) {
  if (h2_esp_target_task_policy_install() == H2_PAL_OK)
    run_host(NULL);
}
