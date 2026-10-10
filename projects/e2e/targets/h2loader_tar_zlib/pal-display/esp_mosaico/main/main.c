#include "device_runner.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_mosaico_display_capture.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include <stdio.h>
#include <string.h>
static h2_runtime_t *runtime;
static esp_timer_handle_t watchdog;
static uint16_t *transferred;
static size_t transfer_count;
static void capture(void *user, const h2_display_rect_t *rect,
                    const uint16_t *pixels) {
  (void)user;
  for (int y = 0; y < rect->height; ++y)
    for (int x = 0; x < rect->width; ++x) {
      uint16_t p = pixels[(size_t)y * rect->width + x];
      transferred[(size_t)(rect->y + y) * 480 + rect->x + x] =
          (uint16_t)((p << 8) | (p >> 8));
    }
  ++transfer_count;
}
static int observe(void *user, const uint16_t *pixels, int width, int height,
                   uint32_t brightness, const char *id) {
  (void)user;
  (void)brightness;
  if (width != 480 || height != 480 || !transfer_count)
    return H2_DISPLAY_ERR_IO;
  int rc = memcmp(pixels, transferred, (size_t)width * height * 2)
               ? H2_DISPLAY_ERR_IO
               : 0;
  printf("H2_DISPLAY_OBSERVATION case=%s source=completed-SPI-DMA chunks=%zu "
         "optical_verified=0 brightness_command=%u rc=%d\n",
         id, transfer_count, (unsigned)brightness, rc);
  return rc;
}
static void fail(const char *stage, int rc) {
  printf("H2_DISPLAY_LAUNCHER_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000));
}
static void deadline(void *unused) {
  (void)unused;
  puts("H2_DISPLAY_WATCHDOG timeout=120s");
  fflush(stdout);
}
static void run(void *unused) {
  (void)unused;
  vTaskDelay(pdMS_TO_TICKS(5000));
  transferred = h2_pal_mem_alloc(runtime->mem, 480u * 480u * 2u);
  if (!transferred)
    fail("capture-allocation", H2_DISPLAY_ERR_NO_MEMORY);
  memset(transferred, 0, 480u * 480u * 2u);
  h2_mosaico_display_set_transfer_capture(capture, NULL);
  int rc = h2_display_device_run(runtime, esp_app_get_description()->version,
                                 observe, NULL, NULL);
  h2_mosaico_display_set_transfer_capture(NULL, NULL);
  h2_pal_mem_free(runtime->mem, transferred);
  transferred = NULL;
  esp_timer_stop(watchdog);
  esp_timer_delete(watchdog);
  if (rc != H2_PAL_OK) {
    printf("H2_DISPLAY_QUALIFICATION_FAIL rc=%d confirm=not-attempted\n", rc);
    fail("qualification", rc);
  }
  int confirm = h2_esp_h2loader_app_confirm(runtime);
  if (confirm != H2_PAL_OK)
    fail("confirm", confirm);
  printf("H2_DISPLAY_READY rc=%d confirm=%d\n", rc, confirm);
  int visual = h2_display_device_demo(runtime);
  if (visual)
    fail("visual-demo", visual);
  visual = h2_display_device_show_pattern(runtime);
  if (visual)
    fail("stable-pattern", visual);
  printf("H2_DISPLAY_STABLE brightness=100 rc=%d optical_verified=0\n", visual);
  for (;;) {
    h2_display_device_replay(runtime);
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc)
    fail("task-policy", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc)
    fail("board", rc);
  config.system_event = h2_pal_unsupported_system_event_api();
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "pal-display",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1,
      .coredump_partition_id = 3};
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                               &commands);
  if (rc)
    fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc)
    fail("runtime", rc);
  const esp_timer_create_args_t timer = {.callback = deadline,
                                         .name = "display-watchdog"};
  if (esp_timer_create(&timer, &watchdog) != ESP_OK ||
      esp_timer_start_once(watchdog, 120000000u) != ESP_OK)
    fail("watchdog", -4);
  const h2_pal_task_options_t options = {.name = "pal-display/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t *task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc)
    fail("runner", rc);
}
