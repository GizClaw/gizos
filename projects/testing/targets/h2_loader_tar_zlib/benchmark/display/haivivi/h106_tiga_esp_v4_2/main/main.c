#include "display_bench.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "h2_device_service.h"
#include "h2_esp_board.h"
#include "h2_esp_target_task_policy.h"

#include <stdio.h>

#define TRANSFER_METRIC "spi_dma_complete"

static h2_device_app_commands_t management;
static h2_runtime_t* runtime;
static uint32_t dma_pixels;
static bool deadline_expired;

static void record_transfer(void* user, const h2_display_rect_t* rect,
                            const uint16_t* pixels) {
  (void)user;
  (void)pixels;
  dma_pixels += (uint32_t)rect->width * rect->height;
}

static uint32_t completed_pixels(void) { return dma_pixels; }

static bool timed_out(void) {
  return __atomic_load_n(&deadline_expired, __ATOMIC_ACQUIRE);
}

static void print_line(const char* line) {
  puts(line);
  fflush(stdout);
}

static void on_deadline(void* user) {
  (void)user;
  __atomic_store_n(&deadline_expired, true, __ATOMIC_RELEASE);
  print_line("H2_DISPLAY_BENCH_RESULT shape=active metric=" TRANSFER_METRIC
             " status=unavailable stage=app_deadline");
}

static int start(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) return rc;
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK) return rc;
  const h2_device_app_commands_config_t commands = {
      .active_name = "display-bench",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 3u,
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u,
      .serial_post_write_poll_ms = 1u,
  };
  rc = h2_device_app_commands_prepare_serial(&management, &config, &commands,
                                             NULL);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) return rc;
  return h2_device_app_confirm(&management, runtime);
}

void app_main(void) {
  const int rc = start();
  printf("H2_DISPLAY_BENCH_CONFIRM rc=%d\n", rc);
  if (rc != H2_PAL_OK) return;

  esp_timer_handle_t timer = NULL;
  const esp_timer_create_args_t deadline = {.callback = on_deadline,
                                            .name = "display-bench"};
  esp_err_t err = esp_timer_create(&deadline, &timer);
  if (err == ESP_OK) err = esp_timer_start_once(timer, 20000000u);
  if (err != ESP_OK) {
    if (timer != NULL) (void)esp_timer_delete(timer);
    printf("H2_DISPLAY_BENCH_END stage=deadline rc=%d\n", err);
    return;
  }

  /* The BSP emits each capture only after its SPI transaction queue drains. */
  h2_esp_board_display_set_transfer_capture(record_transfer, NULL);
  const h2_display_bench_profile_t profile = {
      .board = "tiga_esp_v4_2",
      .version = esp_app_get_description()->version,
      .bus = "SPI",
      .panel_config =
          "panel=ST7789 mode=panel-GRAM spi_host=SPI2 lanes=1 "
          "sck_config_hz=40000000 wire_format=RGB565",
      .transfer_metric = TRANSFER_METRIC,
      .mode = H2_DISPLAY_BENCH_SYNC_TRANSFER,
      .display = runtime->display,
      .memory = runtime->mem,
      .time = runtime->time,
      .line = print_line,
      .completed_pixels = completed_pixels,
      .timed_out = timed_out,
      .clock_tick_ns = 1000u,
  };
  if (h2_display_bench_run(&profile) == H2_DISPLAY_OK)
    (void)h2_display_bench_show_preview(&profile);
  (void)esp_timer_stop(timer);
  (void)esp_timer_delete(timer);
  h2_esp_board_display_set_transfer_capture(NULL, NULL);
}
