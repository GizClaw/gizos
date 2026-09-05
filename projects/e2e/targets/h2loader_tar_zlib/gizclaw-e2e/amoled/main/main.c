#include "h2_gizclaw_e2e_amoled_config.h"
#include "h2_gizclaw_e2e_amoled_state.h"
#include "h2_esp_target_task_policy.h"

#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_gizclaw_e2e.h"
#include "h2_gizclaw_e2e_task_names.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2_runtime_event.h"

#include "esp_system.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define H2_GIZCLAW_E2E_AMOLED_RUNNER_STACK_SIZE 65536u
#define H2_GIZCLAW_E2E_AMOLED_WIFI_STACK_SIZE 8192u
#define H2_GIZCLAW_E2E_AMOLED_EVENT_WAIT_MS 1000u
#define H2_GIZCLAW_E2E_AMOLED_TIME_RETRY_LOG_INTERVAL 10u
#define H2_GIZCLAW_E2E_AMOLED_TIME_SERVER "pool.ntp.org"

extern const uint8_t h2_gizclaw_e2e_voice_prompt_start[]
    asm("_binary_h2_gizclaw_e2e_voice_prompt_start");
extern const uint8_t h2_gizclaw_e2e_voice_prompt_end[]
    asm("_binary_h2_gizclaw_e2e_voice_prompt_end");

typedef struct h2_gizclaw_e2e_amoled_runner {
  h2_runtime_t *runtime;
  h2_gizclaw_e2e_result_t result;
  h2_gizclaw_e2e_exit_t exit_code;
  atomic_bool exited;
} h2_gizclaw_e2e_amoled_runner_t;

typedef struct h2_gizclaw_e2e_amoled_wifi_supervisor {
  h2_runtime_t *runtime;
  const h2_gizclaw_e2e_amoled_config_t *config;
} h2_gizclaw_e2e_amoled_wifi_supervisor_t;

typedef union h2_gizclaw_e2e_amoled_event_payload {
  max_align_t alignment;
  uint8_t bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
} h2_gizclaw_e2e_amoled_event_payload_t;

static h2_gizclaw_e2e_amoled_runner_t s_runner;
static h2_gizclaw_e2e_amoled_wifi_supervisor_t s_wifi_supervisor;

static void hold_for_recovery(void) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000u));
  }
}

static void fail_launcher(const char *stage, int rc,
                          bool command_transport_started) {
  printf("H2_GIZCLAW_E2E_AMOLED stage=%s status=ERROR rc=%d\n", stage, rc);
  fflush(stdout);
  if (!command_transport_started) {
    esp_restart();
  }
  hold_for_recovery();
}

static void emit_progress(void *user,
                          const h2_gizclaw_e2e_progress_t *progress) {
  (void)user;
  if (progress == NULL) {
    return;
  }
  printf("H2_GIZCLAW_E2E kind=%u case=%s status=%s rc=%d "
         "selected=%zu terminal=%zu pass=%zu fail=%zu error=%zu blocked=%zu "
         "cancelled=%zu elapsed_ms=%llu complete=%s\n",
         (unsigned)progress->kind,
         progress->case_id == NULL ? "-" : progress->case_id,
         progress->status == NULL ? "-" : progress->status, progress->rc,
         progress->selected, progress->terminal, progress->passed,
         progress->failed, progress->errors, progress->blocked,
         progress->cancelled, (unsigned long long)progress->elapsed_ms,
         progress->complete ? "true" : "false");
  fflush(stdout);
}

static void emit_summary(const h2_gizclaw_e2e_amoled_runner_t *runner,
                         bool replay) {
  const h2_gizclaw_e2e_result_t *result = &runner->result;
  printf("H2_GIZCLAW_E2E stage=summary entry=bj backend=h2peer suite=all "
         "profile=%s selected=%zu terminal=%zu pass=%zu fail=%zu error=%zu "
         "blocked=%zu cancelled=%zu first_failure_case=%s "
         "first_failure_rc=%d cleanup_rc=%d retained_resources=%zu "
         "complete=%s exit_code=%d replay=%s\n",
         result->runtime_profile_name[0] == '\0'
             ? "-"
             : result->runtime_profile_name,
         result->selected, result->terminal, result->passed, result->failed,
         result->errors, result->blocked, result->cancelled,
         result->first_failure_case[0] == '\0' ? "-"
                                                : result->first_failure_case,
         result->first_failure_rc, result->cleanup_rc,
         result->retained_resources, result->complete ? "true" : "false",
         (int)runner->exit_code, replay ? "true" : "false");
  fflush(stdout);
}

static void run_e2e(void *raw) {
  h2_gizclaw_e2e_amoled_runner_t *runner = raw;
  const h2_gizclaw_e2e_amoled_config_t *launcher_config =
      h2_gizclaw_e2e_amoled_config();
  const h2_gizclaw_e2e_config_t app_config = {
      .server_endpoint = launcher_config->server_endpoint,
      .registration_token = launcher_config->registration_token,
      .voice_pcm_s16le_16khz_mono = h2_gizclaw_e2e_voice_prompt_start,
      .voice_pcm_len = (size_t)(h2_gizclaw_e2e_voice_prompt_end -
                               h2_gizclaw_e2e_voice_prompt_start),
      .suites = H2_GIZCLAW_E2E_SUITE_ALL,
      .case_timeout_ms = H2_GIZCLAW_E2E_DEFAULT_CASE_TIMEOUT_MS,
      .cleanup_timeout_ms = H2_GIZCLAW_E2E_DEFAULT_CLEANUP_TIMEOUT_MS,
      .progress_interval_ms = H2_GIZCLAW_E2E_DEFAULT_PROGRESS_INTERVAL_MS,
      .on_progress = emit_progress,
  };
  runner->exit_code =
      h2_gizclaw_e2e_run(runner->runtime, &app_config, &runner->result);
  atomic_store_explicit(&runner->exited, true, memory_order_release);
}

static void supervise_wifi(void *raw) {
  h2_gizclaw_e2e_amoled_wifi_supervisor_t *supervisor = raw;
  for (;;) {
    h2_gizclaw_e2e_amoled_wifi_result_t result;
    int rc = h2_gizclaw_e2e_amoled_wifi_step(
        supervisor->runtime->wifi_sta, supervisor->runtime->wifi_settings,
        supervisor->config->wifi_connect_timeout_ms, &result);
    if (rc != H2_PAL_OK) {
      printf("H2_GIZCLAW_E2E_AMOLED stage=wifi status=RETRY rc=%d "
             "retry_ms=%u\n",
             rc, (unsigned)supervisor->config->wifi_retry_interval_ms);
      fflush(stdout);
    } else if (result.outcome ==
               H2_GIZCLAW_E2E_AMOLED_WIFI_NO_SAVED_CONFIG) {
      printf("H2_GIZCLAW_E2E_AMOLED stage=wifi status=NO_SAVED_WIFI rc=%d "
             "retry_ms=%u\n",
             result.rc,
             (unsigned)supervisor->config->wifi_retry_interval_ms);
      fflush(stdout);
    } else if (result.outcome == H2_GIZCLAW_E2E_AMOLED_WIFI_CONNECTED) {
      printf("H2_GIZCLAW_E2E_AMOLED stage=wifi status=CONNECTING\n");
      fflush(stdout);
    } else if (result.outcome == H2_GIZCLAW_E2E_AMOLED_WIFI_RETRY) {
      printf("H2_GIZCLAW_E2E_AMOLED stage=wifi status=RETRY rc=%d "
             "retry_ms=%u\n",
             result.rc,
             (unsigned)supervisor->config->wifi_retry_interval_ms);
      fflush(stdout);
    }
    (void)h2_pal_time_sleep_ms(
        supervisor->runtime->time,
        supervisor->config->wifi_retry_interval_ms);
  }
}

static bool event_has_ip(h2_runtime_event_kind_t kind, bool current) {
  if (kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP) {
    return true;
  }
  if (kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED ||
      kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP) {
    return false;
  }
  return current;
}

static void image_entry(void *user) {
  (void)user;
  h2_runtime_config_t runtime_config = {0};
  h2_runtime_t *runtime = NULL;
  h2_pal_task_t *wifi_task = NULL;
  h2_pal_task_t *runner_task = NULL;
  h2_gizclaw_e2e_amoled_state_t state;
  h2_gizclaw_e2e_amoled_state_init(&state);
  const h2_gizclaw_e2e_amoled_config_t *config =
      h2_gizclaw_e2e_amoled_config();

  int rc = h2_esp_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) {
    fail_launcher("runtime_config", rc, false);
  }
  rc = h2_esp_h2loader_app_commands_prepare_serial(&runtime_config,
                                                    "gizclaw-e2e", 1u, 3u);
  if (rc != H2_PAL_OK) {
    fail_launcher("command_prepare", rc, false);
  }
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) {
    fail_launcher("runtime_init", rc, false);
  }
  rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta,
                                      H2_PAL_WIFI_POWER_SAVE_NONE);
  printf("H2_GIZCLAW_E2E_AMOLED stage=power_save mode=%d rc=%d\n",
         (int)H2_PAL_WIFI_POWER_SAVE_NONE, rc);
  fflush(stdout);
  if (rc != H2_PAL_OK) {
    fail_launcher("power_save", rc, false);
  }
  rc = h2_esp_h2loader_app_commands_start(runtime, "gizclaw-e2e", 1u, 3u);
  if (rc != H2_PAL_OK) {
    fail_launcher("command_start", rc, false);
  }

  s_wifi_supervisor = (h2_gizclaw_e2e_amoled_wifi_supervisor_t){
      .runtime = runtime,
      .config = config,
  };
  const h2_pal_task_options_t wifi_options = {
      .name = h2_gizclaw_e2e_wifi_task_name,
      .min_stack_size = H2_GIZCLAW_E2E_AMOLED_WIFI_STACK_SIZE,
  };
  rc = h2_pal_task_start(runtime->task, &wifi_options, supervise_wifi,
                         &s_wifi_supervisor, &wifi_task);
  if (rc != H2_PAL_OK) {
    fail_launcher("wifi_supervisor", rc, true);
  }
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc != H2_PAL_OK) {
    fail_launcher("confirm", rc, true);
  }
  printf("H2_GIZCLAW_E2E_AMOLED stage=launcher status=READY\n");
  fflush(stdout);

  h2_gizclaw_e2e_amoled_event_payload_t payload;
  /* A saved station may already hold an address before this loop sees its
   * first event; seed from the current status instead of waiting for a
   * GOT_IP that was delivered earlier. */
  bool wifi_has_ip = false;
  {
    h2_pal_wifi_sta_status_t wifi_status;
    if (h2_pal_wifi_sta_get_status(runtime->wifi_sta, &wifi_status) ==
            H2_PAL_OK &&
        wifi_status.ip_valid != 0u) {
      wifi_has_ip = true;
    }
  }
  bool sntp_initialized = false;
  bool ble_advertising_paused = false;
  uint32_t ble_pause_retry_count = 0u;
  uint32_t time_retry_count = 0u;
  for (;;) {
    h2_runtime_event_t event = {
        .payload = payload.bytes,
        .payload_capacity = sizeof(payload.bytes),
    };
    rc = h2_runtime_wait_event(runtime, &event,
                               H2_GIZCLAW_E2E_AMOLED_EVENT_WAIT_MS);
    if (rc == H2_PAL_OK) {
      wifi_has_ip = event_has_ip(event.kind, wifi_has_ip);
    } else if (rc != H2_PAL_ERR_TIMEOUT) {
      printf("H2_GIZCLAW_E2E_AMOLED stage=event status=ERROR rc=%d\n", rc);
      fflush(stdout);
    }

    if (!ble_advertising_paused) {
      rc = h2_esp_h2loader_app_commands_pause_ble_advertising();
      if (rc == H2_PAL_OK) {
        ble_advertising_paused = true;
        printf("H2_GIZCLAW_E2E_AMOLED stage=ble_adv status=PAUSED rc=%d\n",
               rc);
        fflush(stdout);
      } else {
        ++ble_pause_retry_count;
        if (ble_pause_retry_count == 1u ||
            ble_pause_retry_count %
                    H2_GIZCLAW_E2E_AMOLED_TIME_RETRY_LOG_INTERVAL ==
                0u) {
          printf("H2_GIZCLAW_E2E_AMOLED stage=ble_adv status=RETRY rc=%d "
                 "retry_ms=%u\n",
                 rc, H2_GIZCLAW_E2E_AMOLED_EVENT_WAIT_MS);
          fflush(stdout);
        }
      }
    }

    if (wifi_has_ip && !state.clock_ready) {
      if (!sntp_initialized) {
        const esp_sntp_config_t sntp_config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG(H2_GIZCLAW_E2E_AMOLED_TIME_SERVER);
        const esp_err_t time_init_rc = esp_netif_sntp_init(&sntp_config);
        if (time_init_rc != ESP_OK) {
          fail_launcher("time_init", (int)time_init_rc, true);
        }
        sntp_initialized = true;
      }
      const esp_err_t time_rc = esp_netif_sntp_sync_wait(
          pdMS_TO_TICKS(H2_GIZCLAW_E2E_AMOLED_EVENT_WAIT_MS));
      if (time_rc == ESP_OK) {
        state.clock_ready = true;
        printf("H2_GIZCLAW_E2E_AMOLED stage=time status=READY\n");
        fflush(stdout);
      } else {
        ++time_retry_count;
        if (time_retry_count == 1u ||
            time_retry_count % H2_GIZCLAW_E2E_AMOLED_TIME_RETRY_LOG_INTERVAL ==
                0u) {
          printf("H2_GIZCLAW_E2E_AMOLED stage=time status=RETRY rc=%d "
                 "retry_ms=%u\n",
                 (int)time_rc, H2_GIZCLAW_E2E_AMOLED_EVENT_WAIT_MS);
          fflush(stdout);
        }
      }
    }
    if (ble_advertising_paused &&
        h2_gizclaw_e2e_amoled_state_set_prerequisites(
            &state, wifi_has_ip, state.clock_ready)) {
      s_runner.runtime = runtime;
      s_runner.result = (h2_gizclaw_e2e_result_t){0};
      s_runner.exit_code = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
      atomic_init(&s_runner.exited, false);
      const h2_pal_task_options_t runner_options = {
          .name = h2_gizclaw_e2e_launcher_task_name,
          .min_stack_size = H2_GIZCLAW_E2E_AMOLED_RUNNER_STACK_SIZE,
      };
      rc = h2_pal_task_start(runtime->task, &runner_options, run_e2e,
                             &s_runner, &runner_task);
      if (rc != H2_PAL_OK) {
        fail_launcher("runner_start", rc, true);
      }
      printf("H2_GIZCLAW_E2E_AMOLED stage=runner status=STARTED\n");
      fflush(stdout);
    }

    uint64_t now_ms = 0u;
    if (runner_task != NULL && !state.runner_complete &&
        atomic_load_explicit(&s_runner.exited, memory_order_acquire)) {
      rc = h2_pal_task_join(runtime->task, runner_task);
      if (rc != H2_PAL_OK) {
        fail_launcher("runner_join", rc, true);
      }
      runner_task = NULL;
      if (h2_pal_time_get_monotonic_ms(runtime->time, &now_ms) != H2_PAL_OK) {
        fail_launcher("summary_clock", H2_PAL_ERR_UNAVAILABLE, true);
      }
      h2_gizclaw_e2e_amoled_state_complete(
          &state, now_ms, config->summary_replay_interval_ms);
      emit_summary(&s_runner, false);
    }
    if (state.runner_complete &&
        h2_pal_time_get_monotonic_ms(runtime->time, &now_ms) == H2_PAL_OK &&
        h2_gizclaw_e2e_amoled_state_take_summary_replay(
            &state, now_ms, config->summary_replay_interval_ms)) {
      emit_summary(&s_runner, true);
    }
  }
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
  h2_pal_result_t rc = h2_esp_board_start_entry_task(
      "amoled/gizclaw-e2e", image_entry, NULL);
  if (rc != H2_PAL_OK) {
    printf("H2_BOARD_ENTRY_FAIL board=devkit image=gizclaw-e2e code=%d\n",
           rc);
  }
}
