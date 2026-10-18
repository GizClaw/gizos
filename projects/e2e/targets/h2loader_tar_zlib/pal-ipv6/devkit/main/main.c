#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_ipv6_device.h"
#include <stdio.h>
#include <string.h>

#ifndef H2_PAL_IPV6_BOARD
#define H2_PAL_IPV6_BOARD "devkit"
#endif

static h2_runtime_t *runtime;
static h2_pal_ipv6_result_t result;
static void hold(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000u));
}
static void fail(const char *stage, int rc) {
  printf("H2_PAL_IPV6_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void run(void *user) {
  (void)user;
  vTaskDelay(pdMS_TO_TICKS(5000u));
  int rc;
  if (h2_pal_ipv6_device_board_fixture()) {
    /* Temporary association: the connect operation never saves credentials. */
    h2_pal_wifi_sta_config_t wifi = {.ssid = "h2ipv6-amoled",
                                     .ssid_len = 13,
                                     .password = "palipv6e2e",
                                     .password_len = 10,
                                     .channel = 6};
    rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &wifi, 20000u);
    memset(&wifi, 0, sizeof(wifi));
  } else {
    rc = h2_runtime_wifi_connect_saved(runtime, 20000u);
  }
  if (rc != H2_PAL_OK)
    fail("saved_wifi", rc);
  uint64_t start = 0u;
  if (h2_pal_time_get_monotonic_ms(runtime->time, &start) != H2_PAL_OK)
    fail("ipv6_clock", H2_PAL_ERR_IO);
  for (;;) {
    h2_pal_net_addr_t local;
    rc = h2_pal_net_get_host_addr_family(runtime->net, NULL,
                                         H2_PAL_NET_FAMILY_IPV6, &local);
    if (rc == H2_PAL_OK && (!h2_pal_ipv6_device_board_fixture() ||
                            (local.ip[0] == 0xfd && local.ip[1] == 0x53))) {
      char line[200];
      snprintf(line, sizeof(line),
               "H2_PAL_IPV6_LINK "
               "local=%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%"
               "02x%02x:%02x%02x peer=%s family=6",
               local.ip[0], local.ip[1], local.ip[2], local.ip[3], local.ip[4],
               local.ip[5], local.ip[6], local.ip[7], local.ip[8], local.ip[9],
               local.ip[10], local.ip[11], local.ip[12], local.ip[13],
               local.ip[14], local.ip[15], h2_pal_ipv6_device_host());
      h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-ipv6", line);
      break;
    }
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND &&
        rc != H2_PAL_ERR_UNAVAILABLE)
      fail("ipv6_address", rc);
    uint64_t now = 0u;
    if (h2_pal_time_get_monotonic_ms(runtime->time, &now) != H2_PAL_OK ||
        now - start >= 20000u)
      fail("ipv6_address", H2_PAL_ERR_UNAVAILABLE);
    vTaskDelay(pdMS_TO_TICKS(100u));
  }
  rc = h2_pal_ipv6_device_run(runtime, h2_esp_platform_dtls_api(), &result);
  if (result.passed + result.failed + result.blocked != H2_PAL_IPV6_CASES)
    fail("ipv6_network_or_fixture", rc);
  int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime)
                                : H2_PAL_ERR_INVALID_STATE;
  for (;;) {
    h2_pal_ipv6_device_report(runtime, &result);
    printf("H2_PAL_IPV6_READY board=%s rc=%d confirm=%d\n", H2_PAL_IPV6_BOARD,
           rc, confirm);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(5000u));
  }
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    fail("task_policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK)
    fail("board", rc);
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "pal-ipv6", 1u, 3u);
  if (rc != H2_PAL_OK)
    fail("command_prepare", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK)
    fail("runtime", rc);
  printf("H2_PAL_IPV6_BOOT board=%s version=%s\n", H2_PAL_IPV6_BOARD,
         esp_app_get_description()->version);
  const h2_pal_task_options_t options = {.name = h2_pal_ipv6_device_task_name};
  h2_pal_task_t *runner = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc != H2_PAL_OK)
    fail("runner", rc);
}
