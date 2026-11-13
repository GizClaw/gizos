/* JieLi AC791N DevKit App image: Runtime lifetime, H2Loader App commands over
 * the USB console, and the game task. The game confirms the trial boot once
 * its first frame is visible with Touch open. Until then a timeout resets the
 * device so that the unconfirmed trial returns to Loader. */
#include "app_config.h"

#include "device/device.h"
#include "os/os_api.h"
#include "system/task.h"
#include "system/timer.h"

#include "h2_atomic.h"
#include "h2_device_service.h"
#include "h2_fruit_slice.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"

#include <stdio.h>

#define CONFIRM_TIMEOUT_MS 120000u

static h2_device_app_commands_t h2_management;
static h2_runtime_t* runtime;
static h2_atomic_uint_t confirmed;
static int commands_ready;

static void return_to_loader(void* user) {
  (void)user;
  if (h2_atomic_load_explicit(&confirmed, H2_ATOMIC_ACQUIRE) != 0u) return;
  printf("H2_FRUIT_SLICE_CONFIRM_TIMEOUT\r\n");
  system_reset();
}

/* Runs before app_main; the SDK calls this weak hook when it is defined. */
int h2_jieli_ac791n_devkit_early_app_boot(void) {
  uint16_t timer = sys_timeout_add_to_task("sys_timer", NULL, return_to_loader,
                                           CONFIRM_TIMEOUT_MS);
  if (timer == 0u) {
    system_reset();
    return -1;
  }
  return 0;
}

static void hold(void) {
  for (;;) os_time_dly(100u);
}

static h2_pal_result_t confirm(void* user) {
  (void)user;
  const int rc = h2_device_app_confirm(&h2_management, runtime);
  if (rc == H2_PAL_OK)
    h2_atomic_store_explicit(&confirmed, 1u, H2_ATOMIC_RELEASE);
  printf("H2_FRUIT_SLICE_CONFIRM rc=%d\r\n", rc);
  return rc;
}

static void game(void* user) {
  (void)user;
  const h2_fruit_slice_config_t config = {.brightness_percent = 100u,
                                          .ready = confirm};
  const int rc = h2_fruit_slice_run(runtime, &config);
  /* The run only ends on failure; keep App commands up for recovery. An
   * unconfirmed run still returns to Loader through the boot timeout. */
  printf("H2_FRUIT_SLICE_SETUP_FAIL stage=game rc=%d\r\n", rc);
  hold();
}

static int start(void) {
  /* Runtime FS needs the SD card the SDK mounts asynchronously. */
  for (unsigned i = 0; !dev_online("sd0") && i < 500u; ++i) os_time_dly(1u);
  h2_runtime_config_t runtime_config;
  int rc = h2_jieli_ac791n_devkit_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) return rc;
  const h2_device_app_commands_config_t commands = {
      .active_name = "fruit-slice",
      .h2loader_partition_id = H2_JIELI_PARTITION_LOADER,
      .app_partition_id = H2_JIELI_PARTITION_APP,
      .coredump_partition_id = H2_JIELI_PARTITION_COREDUMP,
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .app_entry_path = "app/jieli/update.ufw",
      .boot_policy_flags =
          H2_DEVICE_BOOT_TRACK_TRIAL | H2_DEVICE_BOOT_PERSIST_RECOVERY |
          H2_DEVICE_BOOT_STORED_SHADOW | H2_DEVICE_BOOT_PREPARE_SELECTION,
      .identity_policy = H2_DEVICE_IDENTITY_COMMITTED,
      .serial_admission_fallback_ms = H2_DEVICE_APP_ADMISSION_FALLBACK_MS,
      .command_stack_size = 48u * 1024u,
  };
  rc = h2_device_app_commands_prepare_runtime(&h2_management, runtime,
                                              &commands);
  if (rc == H2_PAL_OK)
    rc = h2_device_app_commands_start_serial(&h2_management, NULL);
  if (rc != H2_PAL_OK) return rc;
  commands_ready = 1;
  printf("H2_FRUIT_SLICE_BOOT board=jieli_ac791n_devkit\r\n");
  const h2_pal_task_options_t options = {.name = "fruit-slice/game",
                                         .min_stack_size = 32768u};
  h2_pal_task_t* task = NULL;
  return h2_pal_task_start(runtime->task, &options, game, NULL, &task);
}

/* Return to the SDK app_core event loop once the game task runs. */
void app_main(void) {
  if (h2_atomic_init(&confirmed, 0u) != H2_ATOMIC_OK) {
    system_reset();
    return;
  }
  const int rc = start();
  if (rc == H2_PAL_OK) return;
  printf("H2_FRUIT_SLICE_SETUP_FAIL stage=start rc=%d commands=%d\r\n", rc,
         commands_ready);
  /* Without App commands nothing can recover the device: reset so the
   * unconfirmed trial returns to Loader. Otherwise the boot timeout does. */
  if (!commands_ready) system_reset();
}
