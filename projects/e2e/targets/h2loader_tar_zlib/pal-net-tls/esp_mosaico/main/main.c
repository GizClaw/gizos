#include "h2_mosaico_loader_usb.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_net_tls_device.h"
#include <stdio.h>

#ifndef H2_PAL_NET_TLS_BOARD
#define H2_PAL_NET_TLS_BOARD "esp_mosaico"
#endif

static h2_runtime_t *runtime;
static h2_net_tls_result_t result;
static void hold(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000u));
}
static void fail(const char *stage, int rc) {
  printf("H2_PAL_NET_TLS_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void run(void *user) {
  (void)user;
  vTaskDelay(pdMS_TO_TICKS(5000u));
  int rc;
  uint64_t setup_start = 0u;
  if (h2_pal_time_get_monotonic_ms(runtime->time, &setup_start) != H2_PAL_OK)
    fail("clock", H2_PAL_ERR_IO);
  for (;;) {
    rc = h2_pal_net_tls_device_run(runtime, &result);
    if (result.passed + result.failed + result.blocked + result.unsupported +
            result.not_assessed !=
        0u)
      break;
    if (rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE &&
        rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_BUSY)
      break;
    uint64_t current = 0u;
    if (h2_pal_time_get_monotonic_ms(runtime->time, &current) != H2_PAL_OK ||
        current - setup_start >= 120000u)
      break;
    printf(
        "H2_PAL_NET_TLS_SETUP_WAIT network=not_ready rc=%d cases_started=0\n",
        rc);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000u));
  }
  if (result.passed + result.failed + result.blocked + result.unsupported +
          result.not_assessed !=
      H2_NET_TLS_CASE_COUNT)
    fail("fixture_or_network", rc);
  int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  for (;;) {
    h2_pal_net_tls_device_report(runtime, &result);
    printf("H2_PAL_NET_TLS_READY board=%s rc=%d confirm=%d\n",
           H2_PAL_NET_TLS_BOARD, rc, confirm);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(5000u));
  }
}
void app_main(void) {
  int usb_rc = h2_mosaico_loader_usb_init();
  if (usb_rc != H2_PAL_OK) {
    printf("H2_MOSAICO_USB_FAIL rc=%d\n", usb_rc);
    return;
  }
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    fail("task_policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK)
    fail("board", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "pal-net-tls", 1u,
                                                   3u);
  if (rc != H2_PAL_OK)
    fail("command_prepare", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK)
    fail("runtime", rc);
  printf("H2_PAL_NET_TLS_BOOT board=%s version=%s\n",
         H2_PAL_NET_TLS_BOARD, esp_app_get_description()->version);
  const h2_pal_task_options_t options = {
      .name = h2_pal_net_tls_device_runner_task_name};
  h2_pal_task_t *runner = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc != H2_PAL_OK)
    fail("runner", rc);
}
