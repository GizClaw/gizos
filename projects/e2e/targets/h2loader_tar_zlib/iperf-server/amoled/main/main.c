#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_iperf_server_app.h"
#include "iperf_ap.h"
#include <stdio.h>

static h2_runtime_t *runtime;
static h2_iperf_amoled_ap_t ap;
#ifndef H2_IPERF_SERVER_AUTOSTART_MODE
#define H2_IPERF_SERVER_AUTOSTART_MODE 0
#endif
static h2_iperf_server_app_t *app;
static int ready(void *user) {
  int rc = h2_esp_h2loader_app_confirm(user);
  if (rc == H2_PAL_OK && H2_IPERF_SERVER_AUTOSTART_MODE != 0) {
    printf("H2_IPERF_SERVER_AUTOSTART mode=%u\n",
           (unsigned)H2_IPERF_SERVER_AUTOSTART_MODE);
    fflush(stdout);
    rc = h2_iperf_server_app_request(
        app, (h2_iperf_server_app_mode_t)H2_IPERF_SERVER_AUTOSTART_MODE, true);
  }
  return rc;
}
static void fail(const char *stage, int rc) {
  printf("H2_IPERF_SERVER_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000));
}
static void run(void *unused) {
  (void)unused;
  ap.runtime = runtime;
  const h2_iperf_server_app_config_t config = {
      .network_user = &ap,
      .network_start = h2_iperf_amoled_ap_start,
      .network_stop = h2_iperf_amoled_ap_stop,
      .ui_ready = ready,
      .ui_user = runtime};
  int rc = h2_iperf_server_app_create(runtime, &config, &app);
  if (rc != H2_PAL_OK)
    fail("controller", rc);
  /* Failed Display/Touch startup leaves this image unconfirmed. */
  rc = h2_iperf_server_app_run_ui(app, NULL, NULL);
  int cleanup = h2_iperf_server_app_destroy(&app);
  fail("ui", rc != H2_PAL_OK ? rc : cleanup);
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    fail("task-policy", rc);
  const h2_esp_board_display_config_t display = {.dma_buffer_rows = 8u};
  rc = h2_esp_board_display_configure(&display);
  if (rc != H2_PAL_OK)
    fail("display-policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK)
    fail("board", rc);
  config.mem = h2_esp_platform_psram_allocator();
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "iperf-server",
      .hardware_capabilities =
          H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI,
      .h2loader_partition_id = 1u,
      .coredump_partition_id = 3u};
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                               &commands);
  if (rc != H2_PAL_OK)
    fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK)
    fail("runtime", rc);
  /* Reserve the Wi-Fi driver before Display chooses its internal DMA chunk. */
  rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK)
    fail("wifi-init", rc);
  printf("H2_IPERF_SERVER_BOOT version=%s board=amoled target=esp32s3\n",
         esp_app_get_description()->version);
  const h2_pal_task_options_t options = {.name = "iperf-server/ui",
                                         .min_stack_size = 16384u};
  h2_pal_task_t *task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc != H2_PAL_OK)
    fail("ui-task", rc);
  rc = h2_pal_task_join(runtime->task, task);
  fail("ui-exit", rc);
}
