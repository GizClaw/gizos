#include "h2_esp_board.h"
#include "h2_device_service.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mfg.h"
#include "recording.h"
#include "resources.h"

#include <stdio.h>

extern const h2_mfg_profile_t h2_mfg_board_profile;

static h2_runtime_t* active_runtime;
static h2_device_loader_t device_loader;

static int run_mfg(void* user, h2_loader_t* loader,
                   const h2_pal_sync_api_t* operation_sync,
                   h2_pal_mutex_t* operation_mutex,
                   const h2_pal_sync_api_t* wifi_sync,
                   h2_pal_mutex_t* wifi_mutex) {
  h2_passport_recording_bind(active_runtime, loader);
  h2_passport_resources_bind_wifi(wifi_sync, wifi_mutex);
  return h2_mfg_run(user, loader, operation_sync, operation_mutex, wifi_sync,
                    wifi_mutex);
}

static int before_disruptive(void* user, h2_loader_disruptive_action_t action) {
  int rc = h2_mfg_before_disruptive(user, action);
  /* The BLE service may be carrying this command; its owner closes it after
   * the command returns. Audio has no remaining users once MFG has joined. */
  return rc == 0 ? h2_esp_board_audio_release() : rc;
}

static void image_entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config;
  h2_runtime_t* runtime = NULL;
  h2_mfg_t* mfg = NULL;
  int rc = h2_esp_board_runtime_config(&config);
  if (rc == 0) {
    /* MFG polls its keys/battery directly; no Runtime input sources register.
     */
    config.input_source_capacity = 1;
    config.component_mapping_capacity = 8;
    config.event_queue_capacity = 4;
    rc = h2_runtime_init(&config, &runtime);
  }
  if (rc == 0) rc = h2_passport_resources_init(runtime);
  if (rc == 0) rc = h2_mfg_create(runtime, &h2_mfg_board_profile, &mfg);
  if (rc != 0) {
    printf("H2_MFG_INIT_FAIL board=%s code=%d\n", h2_mfg_board_profile.board,
           rc);
    if (runtime != NULL) h2_runtime_deinit(runtime);
    return;
  }
  active_runtime = runtime;
  h2_device_loader_config_t loader = {
      .board = h2_mfg_board_profile.board,
      .loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 3u,
      .hardware_capabilities = H2_LOADER_CAPABILITIES_ALL,
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u,
      .command_task_stack_size = 20480,
      .serial_rx_buffer_size = 1024,
      .serial_receive_window = 8,
      .mfg_required_total = h2_mfg_board_profile.total,
      .user = mfg,
      .mfg_command = h2_mfg_command,
      .run_mfg = run_mfg,
      .rearm_mfg = h2_mfg_rearm,
      .before_disruptive = before_disruptive,
      .poll_command_service = h2_passport_resources_poll,
      .stop_command_service = h2_passport_resources_stop,
  };
  rc = h2_device_loader_run(&device_loader, runtime, &loader, NULL, 0);
  if (rc != 0) printf("H2_MFG_LOADER_FAIL code=%d\n", rc);
  while ((rc = h2_mfg_destroy(&mfg)) != 0) {
    printf("H2_MFG_CLEANUP_FAIL code=%d\n", rc);
    (void)h2_pal_time_sleep_ms(runtime->time, 1000);
  }
  h2_passport_recording_bind(NULL, NULL);
  h2_passport_resources_bind_wifi(NULL, NULL);
  active_runtime = NULL;
  h2_runtime_deinit(runtime);
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc == 0) {
    image_entry(NULL);
    rc = h2_esp_board_runtime_deinit();
  }
  if (rc != 0) printf("H2_MFG_ENTRY_FAIL code=%d\n", rc);
}
