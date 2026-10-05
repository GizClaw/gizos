#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "ipv6_ap.h"
#include "tls_server.h"
#include <stdio.h>
static h2_runtime_t *runtime;
static void hold(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char *stage, int rc) {
  printf("H2_IPV6_FIXTURE_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static int confirm(h2_runtime_t *rt) {
  int rc = h2_esp_h2loader_app_confirm(rt);
  printf("H2_IPV6_FIXTURE_CONFIRM session=%s rc=%d\n",
         h2_ipv6_fixture_session(), rc);
  fflush(stdout);
  return rc;
}
static void run(void *user) {
  (void)user;
  int rc = h2_ipv6_ap_start(runtime);
  if (rc)
    fail("ap_ipv6", rc);
  rc = h2_ipv6_board_fixture_run(runtime, h2_esp_platform_dtls_api(),
                                 &h2_ipv6_tls_server, confirm);
  int cleaned = h2_ipv6_ap_stop(runtime);
  if (cleaned)
    fail("ap_cleanup", cleaned);
  fail("services", rc);
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc)
    fail("task_policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc)
    fail("board", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "pal-ipv6-fixture",
                                                   1, 3);
  if (rc)
    fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc)
    fail("runtime", rc);
  printf("H2_IPV6_FIXTURE_BOOT board=amoled version=%s session=%s\n",
         esp_app_get_description()->version, h2_ipv6_fixture_session());
  const h2_pal_task_options_t options = {.name = h2_ipv6_fixture_task_name};
  h2_pal_task_t *task;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc)
    fail("runner", rc);
}
