#include "h2_pal_e2e_application_host.h"
#ifdef H2_PAL_AUDIO_BOARD_HOST_TEST
#include "h2_pal_audio_board_test_sdk.h"
#else
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_pal_audio_e2e.h"
#endif

#include <stdio.h>

static const h2_pal_audio_api_t *provider_audio;
static h2_pal_audio_e2e_result_t result;

static void report(void *user, const h2_pal_audio_e2e_case_result_t *item) {
  (void)user;
  printf("H2_PAL_AUDIO_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
         item->id, item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
         item->detail, item->line);
  fflush(stdout);
}

static void recover_failed_run(int rc, int confirm) {
  printf("H2_PAL_AUDIO_RECOVERY board=amoled rc=%d confirm=%d\n", rc, confirm);
  fflush(stdout);
  h2_loader_app_client_config_t recovery_config;
  h2_loader_app_client_t client;
  int recovery = h2_esp_h2loader_app_commands_get_config(&recovery_config);
  int locked = 0;
  if (recovery == H2_PAL_OK && recovery_config.operation_mutex != NULL) {
    recovery = h2_pal_mutex_lock(recovery_config.operation_sync,
                                  recovery_config.operation_mutex);
    locked = recovery == H2_PAL_OK;
  }
  if (recovery == H2_PAL_OK) {
    recovery = h2_loader_app_client_init(&client, &recovery_config);
    if (recovery == H2_PAL_OK) {
      recovery = h2_loader_reboot_h2loader_with_transition(&client.loader,
                                                            NULL, NULL);
      h2_loader_deinit(&client.loader);
    }
  }
  if (locked)
    (void)h2_pal_mutex_unlock(recovery_config.operation_sync,
                               recovery_config.operation_mutex);
  printf("H2_PAL_AUDIO_RECOVERY board=amoled loader_reboot=%d\n", recovery);
  fflush(stdout);
  /* Reboot normally does not return. If it does, reset the whole chip so
   * failed playback cannot remain active in a terminal reporting loop. */
  vTaskDelay(pdMS_TO_TICKS(250u));
  esp_restart();
}

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  const h2_pal_audio_e2e_config_t config = {
      .audio = provider_audio, .time = runtime->time,
      .stability_ms = 30000u, .report = report};
  const int rc = h2_pal_audio_e2e_run(&config, &result);
  const int confirm = rc == H2_AUDIO_OK
                          ? h2_esp_h2loader_app_confirm(runtime)
                          : H2_PAL_ERR_INVALID_STATE;
  if (rc != H2_AUDIO_OK || confirm != H2_PAL_OK) {
    recover_failed_run(rc, confirm);
    return rc;
  }
  while (!h2_application_host_should_stop(host)) {
    /* H2Loader control and serial logs share a transport. Repeat the stored
     * terminal ledger so one interrupted UART line cannot hide a case. */
    for (size_t i = 0u; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i) {
      const h2_pal_audio_e2e_case_result_t *item = &result.cases[i];
      printf("H2_PAL_AUDIO_REPLAY {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d}\n",
             item->id, item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
             item->detail);
      fflush(stdout);
      vTaskDelay(pdMS_TO_TICKS(10u));
    }
    printf("H2_PAL_AUDIO_SUMMARY {\"platform\":\"amoled\",\"passed\":%u,"
           "\"failed\":%u,\"blocked\":%u,\"mic_frames\":%u,"
           "\"mic_peak\":%u,\"mic_energy\":%llu,\"speaker_frames\":%u,"
           "\"output_peak\":%u,\"stability_elapsed_ms\":%llu,"
           "\"rc\":%d,\"confirm\":%d}\n",
           result.passed, result.failed, result.blocked,
           (unsigned)result.mic_frames, (unsigned)result.mic_peak,
           (unsigned long long)result.mic_energy,
           (unsigned)result.speaker_frames, (unsigned)result.output_peak,
           (unsigned long long)result.stability_elapsed_ms,
           rc, confirm);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(5000u));
  }

  return rc;
}

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  (void)config;
  provider_audio = config->audio;

  return H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  (void)user;
  (void)runtime;
  printf("H2_PAL_AUDIO_BOOT board=amoled version=%s\n",
         esp_app_get_description()->version);

  return H2_PAL_OK;
}

void app_main(void) {
    const h2_application_host_config_t app = {
        .entry = run, .configure = configure, .validate = validate};
    (void)h2_pal_e2e_application_host_start(
        "pal-audio", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI | H2_LOADER_CAPABILITY_BLE, &app);
}
