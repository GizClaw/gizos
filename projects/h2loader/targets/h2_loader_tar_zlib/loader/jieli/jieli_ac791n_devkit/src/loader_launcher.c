#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_console.h"
#include "h2_device_service.h"
#include "h2loader_board_extension.h"

#include <stdio.h>

/* Only SDK boot, board configuration and boot-lifetime ownership live here. */
const uint32_t h2_jieli_wl82_coredump_loader_image = 1u;
static h2_device_loader_t loader;
static h2_pal_task_t* startup_worker;
static int started;
static int crash_recovery_active;

static void sleep_ms(uint32_t ms) {
  (void)h2_pal_time_sleep_ms(h2_jieli_wl82_platform_time_api(), ms);
}
static void error_forever(const char* step, int rc) {
  char text[128];
  (void)snprintf(text, sizeof(text),
                 "H2_JIELI_LOADER_ERROR step=%s code=%d\r\n", step, rc);
  for (;;) {
    h2_jieli_wl82_diag_write(text);
    sleep_ms(1000u);
  }
}
void h2_jieli_sd_fs_trace_write(const char* phase, size_t offset, size_t length,
                                int result) {
  char text[160];
  (void)snprintf(text, sizeof(text),
                 "H2_JIELI_SD_WRITE phase=%s offset=%u length=%u result=%d\r\n",
                 phase != NULL ? phase : "unknown", (unsigned)offset,
                 (unsigned)length, result);
  h2_jieli_wl82_diag_write(text);
}
void h2_jieli_usb_debug_heartbeat(void) {
#ifndef H2_JIELI_BLE_DIAG_QUIET_UART
  if (!started) return;
  uint64_t now = 0u;
  (void)h2_pal_time_get_monotonic_ms(h2_jieli_wl82_platform_time_api(), &now);
  char text[192];
  (void)snprintf(text, sizeof(text),
                 "H2_JIELI_LINK ms=%u last_frame_ms=%u fs=%s\r\n",
                 (unsigned)now, (unsigned)loader.serial.last_frame_ms,
                 h2_jieli_ac791n_devkit_sd_fs_last_stage());
  h2_jieli_wl82_diag_write(text);
#endif
}
__attribute__((weak)) void h2_jieli_loader_pref_probe(
    const h2_pal_pref_api_t* pref, const h2_pal_mem_api_t* mem) {
  (void)pref;
  (void)mem;
}
__attribute__((weak)) int h2loader_board_extend_runtime(
    h2_runtime_config_t* config) {
  (void)config;
  return H2_PAL_OK;
}
__attribute__((weak)) int h2loader_board_extend_app(
    h2_runtime_t* runtime, h2loader_app_config_t* config) {
  (void)runtime;
  (void)config;
  return H2_PAL_OK;
}
static void command_ready(void* user, h2_loader_t* state) {
  (void)user;
  h2_jieli_loader_pref_probe(state->config.pref,
                             state->config.package.allocator);
}
static void loader_task(void* user) {
  (void)user;
  sleep_ms(100u);
  h2_runtime_config_t config;
  int rc;
  for (;;) {
    rc = h2_jieli_ac791n_devkit_runtime_config(&config);
    if (rc == H2_PAL_OK) break;
    if (rc != H2_PAL_ERR_UNAVAILABLE && rc != H2_PAL_ERR_IO)
      error_forever("runtime_config", rc);
    sleep_ms(500u);
  }
  (void)h2_jieli_wl82_coredump_flush_pending();
  rc = h2loader_board_extend_runtime(&config);
  if (rc != H2_PAL_OK) error_forever("board_runtime", rc);
  h2_runtime_t* runtime = NULL;
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) error_forever("runtime_init", rc);
  const h2_device_loader_config_t service = {
      .board = runtime->board,
      .app_entry_path = "app/jieli/update.ufw",
      .loader_partition_id = H2_JIELI_PARTITION_LOADER,
      .app_partition_id = H2_JIELI_PARTITION_APP,
      .coredump_partition_id = H2_JIELI_PARTITION_COREDUMP,
      .hardware_capabilities =
          H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
      .boot_policy_flags =
          H2_DEVICE_BOOT_TRACK_TRIAL | H2_DEVICE_BOOT_PERSIST_RECOVERY |
          H2_DEVICE_BOOT_STORED_SHADOW | H2_DEVICE_BOOT_PREPARE_SELECTION |
          H2_DEVICE_BOOT_FACTORY_IDENTITY,
      .identity_policy = H2_DEVICE_IDENTITY_COMMITTED,
      .serial_idle_timeout_ms = 30000u,
      .force_command_mode = crash_recovery_active,
      .ble_advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
      .extend_app = h2loader_board_extend_app,
      .command_ready = command_ready,
  };
  rc = h2_device_loader_run(&loader, runtime, &service, NULL,
                            !crash_recovery_active);
  error_forever("app_exit", rc);
}
void app_main(void) {
  if (started) return;
  int rc = h2_jieli_wl82_console_start();
  if (rc != H2_PAL_OK) return;
  crash_recovery_active = h2_jieli_wl82_take_loader_crash_pending() != 0;
  const h2_pal_task_options_t options = {.name = "h2loader/startup",
                                         .min_stack_size = 16384u};
  rc = h2_pal_task_start(h2_jieli_wl82_platform_task_api(), &options,
                         loader_task, NULL, &startup_worker);
  if (rc == H2_PAL_OK)
    started = 1;
  else
    h2_jieli_wl82_diag_write("H2_JIELI_LOADER_ERROR step=portable_start\r\n");
}
