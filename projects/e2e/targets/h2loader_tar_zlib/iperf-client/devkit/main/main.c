#include "result_console.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_iperf_client_app.h"
#include <stdio.h>
#include <string.h>

static h2_runtime_t *runtime;
static h2_esp_e2e_result_log_t result_log;
static int confirm_app(void *user) { return h2_esp_h2loader_app_confirm(user); }
static void hold(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000u));
}
static void fail(const char *stage, int rc) {
  printf("H2_IPERF_CLIENT_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void memory(void *user, const char *checkpoint) {
  (void)user;
  printf(
      "H2_IPERF_CLIENT_MEMORY checkpoint=%s internal_free=%u "
      "internal_min=%u psram_free=%u\n",
      checkpoint,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  fflush(stdout);
}
static void run(void *user) {
  (void)user;
  memory(NULL, "matrix-start");
  int rc = h2_iperf_client_app_bench(runtime, runtime->board, memory, NULL);
  memory(NULL, "matrix-end");
  if (rc != H2_PAL_OK)
    fail("bench", rc);
  rc = h2_esp_e2e_result_log_confirm(&result_log, rc, 0u, confirm_app, runtime);
  if (rc != H2_PAL_OK)
    fail("confirm", rc);
  printf("H2_IPERF_CLIENT_CONFIRMED rc=%d\n", rc);
  fflush(stdout);
  hold();
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK)
    fail("task-policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK)
    fail("board", rc);
  config.mem = h2_esp_platform_psram_allocator();
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "iperf-client",
      .hardware_capabilities =
          H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI,
      .h2loader_partition_id = 1u,
      .coredump_partition_id = 3u};
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                               &commands);
  if (rc != H2_PAL_OK)
    fail("command-prepare", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK)
    fail("runtime", rc);
  rc = h2_esp_e2e_result_log_prepare(&result_log, runtime, "iperf-client", "iperf_e2e");
  if (rc != H2_PAL_OK)
    fail("result_console", rc);
  printf("H2_IPERF_CLIENT_BOOT board=%s version=%s\n",
         runtime->board, esp_app_get_description()->version);
  fflush(stdout);
  const h2_pal_task_options_t options = {.name = h2_iperf_client_app_task_name,
                                         .min_stack_size = 16384u};
  h2_pal_task_t *task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc != H2_PAL_OK)
    fail("task-start", rc);
  rc = h2_pal_task_join(runtime->task, task);
  fail("task-exit", rc);
}
