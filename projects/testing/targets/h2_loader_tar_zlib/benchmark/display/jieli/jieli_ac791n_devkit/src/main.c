#include "app_config.h"
#include "asm/clock.h"
#include "device/device.h"
#include "os/os_api.h"
#include "system/task.h"
#include "system/timer.h"

#include "display_bench.h"
#include "h2_atomic.h"
#include "h2_device_service.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"

#include <stdio.h>

static h2_device_app_commands_t management;
static h2_runtime_t* runtime;
static h2_atomic_uint_t confirmed;

static void print_line(const char* line) { printf("%s\r\n", line); }

static void return_to_loader(void* user) {
  (void)user;
  if (h2_atomic_load_explicit(&confirmed, H2_ATOMIC_ACQUIRE) == 0u)
    system_reset();
}

int h2_jieli_ac791n_devkit_early_app_boot(void) {
  if (sys_timeout_add_to_task("sys_timer", NULL, return_to_loader, 120000u) !=
      0u)
    return 0;
  system_reset();
  return -1;
}

static void run(void* user) {
  (void)user;
  const int confirm = h2_device_app_confirm(&management, runtime);
  printf("H2_DISPLAY_BENCH_CONFIRM rc=%d\r\n", confirm);
  if (confirm != H2_PAL_OK) return;
  h2_atomic_store_explicit(&confirmed, 1u, H2_ATOMIC_RELEASE);
  const int osc_hz = clk_get("osc");
  const h2_display_bench_profile_t profile = {
      .board = "jieli_ac791n_devkit",
      .version = H2_JIELI_FIRMWARE_VERSION,
      .bus = "8080-EMI",
      .panel_config =
          "panel=ILI9481_or_ILI9488 width_bits=8 emi_baud_div=8 "
          "emi_tw_div=8 edge=falling",
      .mode = H2_DISPLAY_BENCH_CLOSE_FENCE,
      .display = runtime->display,
      .memory = runtime->mem,
      .time = runtime->time,
      .line = print_line,
      .clock_tick_ns = osc_hz > 0 ? (unsigned)(8192000000000ull / osc_hz) : 0u,
  };
  const int rc = h2_display_bench_run(&profile);
  if (rc == H2_DISPLAY_OK) {
    (void)h2_display_bench_show_preview(&profile);
  } else {
    printf("H2_DISPLAY_BENCH_PREVIEW status=skipped stage=benchmark rc=%d\r\n",
           rc);
  }
}

static int start(void) {
  for (unsigned i = 0; !dev_online("sd0") && i < 500u; ++i) os_time_dly(1u);
  h2_runtime_config_t config;
  int rc = h2_jieli_ac791n_devkit_runtime_config(&config);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) return rc;
  const h2_device_app_commands_config_t commands = {
      .active_name = "display-bench",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = H2_JIELI_PARTITION_LOADER,
      .app_partition_id = H2_JIELI_PARTITION_APP,
      .coredump_partition_id = H2_JIELI_PARTITION_COREDUMP,
      .app_entry_path = "app/jieli/update.ufw",
      .boot_policy_flags =
          H2_DEVICE_BOOT_TRACK_TRIAL | H2_DEVICE_BOOT_PERSIST_RECOVERY |
          H2_DEVICE_BOOT_STORED_SHADOW | H2_DEVICE_BOOT_PREPARE_SELECTION,
      .identity_policy = H2_DEVICE_IDENTITY_COMMITTED,
      .serial_admission_fallback_ms = H2_DEVICE_APP_ADMISSION_FALLBACK_MS,
      .command_stack_size = 48u * 1024u,
  };
  rc = h2_device_app_commands_prepare_runtime(&management, runtime, &commands);
  if (rc == H2_PAL_OK)
    rc = h2_device_app_commands_start_serial(&management, NULL);
  if (rc != H2_PAL_OK) return rc;
  const h2_pal_task_options_t options = {.name = "display-bench/run",
                                         .min_stack_size = 16384u};
  h2_pal_task_t* task = NULL;
  return h2_pal_task_start(runtime->task, &options, run, NULL, &task);
}

void app_main(void) {
  if (h2_atomic_init(&confirmed, 0u) != H2_ATOMIC_OK) {
    system_reset();
    return;
  }
  const int rc = start();
  if (rc != H2_PAL_OK) {
    printf("H2_DISPLAY_BENCH_END stage=start rc=%d\r\n", rc);
    system_reset();
  }
}
