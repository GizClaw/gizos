#include "h2_gizclaw_e2e_amoled_config.h"
#include "h2_gizclaw_e2e_amoled_ota.h"
#include "h2_gizclaw_e2e_amoled_state.h"
#include "h2_esp_target_task_policy.h"

#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_gizclaw_e2e.h"
#include "h2_gizclaw_e2e_task_names.h"
#include "ledger.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2_runtime_event.h"

#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_debug_helpers.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdalign.h>
#include "h2_atomic.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#define H2_GIZCLAW_E2E_AMOLED_RUNNER_STACK_SIZE 65536u
#define H2_GIZCLAW_E2E_AMOLED_WIFI_STACK_SIZE 8192u
#define H2_GIZCLAW_E2E_AMOLED_EVENT_WAIT_MS 1000u
#define H2_GIZCLAW_E2E_AMOLED_TIME_RETRY_LOG_INTERVAL 10u
#define H2_GIZCLAW_E2E_LEDGER_CAPACITY (384u * 1024u)

extern const uint8_t h2_gizclaw_e2e_voice_prompt_start[]
    asm("_binary_h2_gizclaw_e2e_voice_prompt_start");
extern const uint8_t h2_gizclaw_e2e_voice_prompt_end[]
    asm("_binary_h2_gizclaw_e2e_voice_prompt_end");

typedef struct h2_gizclaw_e2e_amoled_runner {
  h2_runtime_t *runtime;
  h2_gizclaw_e2e_config_t app_config;
  h2_gizclaw_e2e_result_t result;
  h2_gizclaw_e2e_exit_t exit_code;
  h2_atomic_bool_t exited;
  h2_atomic_bool_t capture_failed;
  h2_pal_mutex_t *evidence_mutex;
  h2_gizclaw_e2e_ledger_t ledger;
  char execution[33];
  bool admitted;
  int confirm_rc;
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
static bool s_commands_started;

static void capture_evidence(void *user, const char *record, size_t size) {
  h2_gizclaw_e2e_amoled_runner_t *runner = user;
  if (h2_pal_mutex_lock(runner->runtime->sync, runner->evidence_mutex) != H2_PAL_OK) {
    h2_atomic_store_explicit(&runner->capture_failed, true, H2_ATOMIC_RELEASE);
    return;
  }
  h2_gizclaw_e2e_ledger_append(&runner->ledger, record, size);
  if (runner->ledger.error != H2_PAL_OK)
    h2_atomic_store_explicit(&runner->capture_failed, true, H2_ATOMIC_RELEASE);
  if (h2_pal_mutex_unlock(runner->runtime->sync, runner->evidence_mutex) != H2_PAL_OK)
    h2_atomic_store_explicit(&runner->capture_failed, true, H2_ATOMIC_RELEASE);
}

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

#if defined(H2_GIZCLAW_E2E_RESOURCE_ONLY)
#define AMOLED_E2E_SUITES H2_GIZCLAW_E2E_SUITE_RESOURCE
#define AMOLED_E2E_SUITE_NAME "resource"
#elif defined(H2_GIZCLAW_E2E_VOICE_ONLY)
#define AMOLED_E2E_SUITES H2_GIZCLAW_E2E_SUITE_VOICE
#define AMOLED_E2E_SUITE_NAME "voice"
#elif defined(H2_GIZCLAW_E2E_RPC_ONLY)
#define AMOLED_E2E_SUITES H2_GIZCLAW_E2E_SUITE_RPC
#define AMOLED_E2E_SUITE_NAME "rpc"
#elif defined(H2_GIZCLAW_E2E_DEVICE_ONLY)
#define AMOLED_E2E_SUITES H2_GIZCLAW_E2E_SUITE_DEVICE
#define AMOLED_E2E_SUITE_NAME "device"
#else
#define AMOLED_E2E_SUITES H2_GIZCLAW_E2E_SUITE_ALL
#define AMOLED_E2E_SUITE_NAME "all"
#endif

static int fixture_body(void *user, const h2_pal_http_request_t *request,
                        const uint8_t *chunk, size_t size, size_t read,
                        size_t remaining) {
  (void)user; (void)request; (void)chunk; (void)size; (void)read; (void)remaining;
  return H2_PAL_OK;
}

/* Read-only setup: warm real DNS/TLS before registering any test identity.
 * These are readiness observations; the suite still asserts actual business
 * operations and the full audio payload/Track lifecycle afterwards. */
static int network_fixture_ready(h2_runtime_t *runtime) {
  char info[160];
  int size = snprintf(info, sizeof(info), "http://%s/server-info",
                      h2_gizclaw_e2e_fixture_endpoint());
  if (size <= 0 || (size_t)size >= sizeof(info))
    return H2_PAL_ERR_INVALID_ARG;
  const char *uris[] = {info, h2_gizclaw_e2e_fixture_device_api_url(),
                        h2_gizclaw_e2e_fixture_audio_url()};
  for (unsigned i = 0u; i < 3u; ++i) {
    if (i != 0u && (AMOLED_E2E_SUITES & H2_GIZCLAW_E2E_SUITE_DEVICE) == 0u)
      continue;
    uint8_t chunk[512];
    h2_pal_http_request_t request = {
        .method = i == 0u ? H2_PAL_HTTP_GET : H2_PAL_HTTP_HEAD,
        .url = {uris[i], strlen(uris[i])}, .timeout_ms = 10000u,
        .allocator = runtime->mem, .chunk_buf = chunk, .chunk_buf_cap = sizeof(chunk),
        .read_cb = fixture_body,
    };
    h2_pal_http_response_t response;
    h2_pal_http_response_reset(&response);
    int rc = h2_pal_http_request(runtime->http, &request, &response);
    /* The unauthenticated API root may return 401/404; we require its verified
     * HTTP response, not a successful privileged operation. */
    if (rc == H2_PAL_OK && (response.status_code < 200 || response.status_code >= 500 ||
        (i != 1u && response.status_code != 200)))
      rc = H2_PAL_ERR_IO;
    printf("H2_GIZCLAW_SETUP network_fixture=%u http=%d rc=%d\n", i, response.status_code, rc);
    h2_pal_http_response_free(runtime->http, &response);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}

static int format_summary(const h2_gizclaw_e2e_amoled_runner_t *runner,
                          bool replay, char *buffer, size_t capacity) {
  const h2_gizclaw_e2e_result_t *result = &runner->result;
  const h2_gizclaw_e2e_amoled_config_t *settings = h2_gizclaw_e2e_amoled_config();
  return snprintf(buffer, capacity, "H2_GIZCLAW_E2E stage=%s platform=amoled endpoint=%.*s backend=h2peer suite=" AMOLED_E2E_SUITE_NAME " "
         "profile=%s selected=%zu terminal=%zu pass=%zu fail=%zu error=%zu "
         "blocked=%zu cancelled=%zu first_failure_case=%s "
         "first_failure_rc=%d cleanup_rc=%d retained_resources=%zu "
         "complete=%s exit_code=%d replay=%s\n",
         replay ? "summary-replay" : "summary",
         (int)settings->server_endpoint.len, settings->server_endpoint.data,
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
}

static void emit_summary(const h2_gizclaw_e2e_amoled_runner_t *runner, bool replay) {
  char buffer[1024];
  int size = format_summary(runner, replay, buffer, sizeof(buffer));
  if (size > 0 && (size_t)size < sizeof(buffer))
    fputs(buffer, stdout);
  fflush(stdout);
}

static void replay_ledger(const h2_gizclaw_e2e_amoled_runner_t *runner) {
  if (!runner->ledger.frozen)
    return;
  const esp_app_desc_t *description = esp_app_get_description();
  printf("H2_GIZCLAW_LEDGER stage=begin version=%s execution=%s bytes=%zu records=%zu crc32=%08x admitted=%d confirm_rc=%d physical_audio=%d\n",
         description->version, runner->execution, runner->ledger.size,
         runner->ledger.records, (unsigned)runner->ledger.crc32,
         runner->admitted ? 1 : 0, runner->confirm_rc, h2_gizclaw_e2e_fixture_physical_audio());
  fwrite(runner->ledger.data, 1u, runner->ledger.size, stdout);
  printf("H2_GIZCLAW_LEDGER stage=end execution=%s crc32=%08x\n",
         runner->execution, (unsigned)runner->ledger.crc32);
  fflush(stdout);
}

static void run_e2e(void *raw) {
  h2_gizclaw_e2e_amoled_runner_t *runner = raw;
  volatile uint8_t stack_probe = 0u;
  const bool stack_in_psram = esp_ptr_external_ram((const void *)&stack_probe);
  printf("H2_GIZCLAW_E2E_AMOLED stage=runner_stack region=%s "
         "status=%s\n",
         stack_in_psram ? "psram" : "other",
         stack_in_psram ? "PASS" : "ERROR");
  fflush(stdout);
  if (!stack_in_psram) {
    runner->exit_code = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
    h2_atomic_store_explicit(&runner->exited, true, H2_ATOMIC_RELEASE);
    return;
  }
#if defined(H2_GIZCLAW_E2E_OTA_ONLY)
  h2_gizclaw_e2e_amoled_ota_run(runner->runtime);
  return;
#endif
  const h2_gizclaw_e2e_amoled_config_t *launcher_config =
      h2_gizclaw_e2e_amoled_config();
  runner->app_config = (h2_gizclaw_e2e_config_t){
      .server_endpoint = launcher_config->server_endpoint,
      .registration_token = launcher_config->registration_token,
      .app_config_key = h2_gizclaw_e2e_fixture_key(),
      .expected_runtime_profile = h2_gizclaw_e2e_fixture_profile(),
      .app_config_expected_value = {h2_gizclaw_e2e_fixture_value(),
                                   strlen(h2_gizclaw_e2e_fixture_value())},
      .voice_audio = h2_gizclaw_e2e_fixture_physical_audio()
                         ? runner->runtime->audio : NULL,
      .voice_pcm_s16le_16khz_mono = h2_gizclaw_e2e_voice_prompt_start,
      .voice_pcm_len = (size_t)(h2_gizclaw_e2e_voice_prompt_end -
                               h2_gizclaw_e2e_voice_prompt_start),
      .suites = AMOLED_E2E_SUITES,
      .device_api_url = h2_gizclaw_e2e_fixture_device_api_url(),
      .device_audio_url = h2_gizclaw_e2e_fixture_audio_url(),
      .device_real_audio = h2_gizclaw_e2e_fixture_physical_audio() != 0,
      .case_timeout_ms = H2_GIZCLAW_E2E_DEFAULT_CASE_TIMEOUT_MS,
      .cleanup_timeout_ms = H2_GIZCLAW_E2E_DEFAULT_CLEANUP_TIMEOUT_MS,
      .progress_interval_ms = H2_GIZCLAW_E2E_DEFAULT_PROGRESS_INTERVAL_MS,
      .on_progress = emit_progress,
      .on_evidence = capture_evidence,
      .evidence_user = runner,
  };
  runner->exit_code =
      h2_gizclaw_e2e_run(runner->runtime, &runner->app_config, &runner->result);
  h2_atomic_store_explicit(&runner->exited, true, H2_ATOMIC_RELEASE);
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

  if (!h2_gizclaw_e2e_fixture_endpoint()[0] || !h2_gizclaw_e2e_fixture_token()[0] ||
      !h2_gizclaw_e2e_fixture_profile()[0] || !h2_gizclaw_e2e_fixture_time_server()[0] ||
      ((AMOLED_E2E_SUITES & H2_GIZCLAW_E2E_SUITE_DEVICE) != 0u &&
       (!h2_gizclaw_e2e_fixture_device_api_url()[0] || !h2_gizclaw_e2e_fixture_audio_url()[0])) ||
      ((AMOLED_E2E_SUITES & H2_GIZCLAW_E2E_SUITE_RPC) != 0u &&
       (!h2_gizclaw_e2e_fixture_key()[0] || !h2_gizclaw_e2e_fixture_value()[0])))
    fail_launcher("missing_service_fixture", H2_PAL_ERR_INVALID_ARG, false);

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
  /* Dedicated OTA preparation keeps its original command lifecycle. Ordinary
   * qualification starts command serving after case tasks have quiesced. */
#if defined(H2_GIZCLAW_E2E_OTA_ONLY)
  rc = h2_esp_h2loader_app_commands_start(runtime, "gizclaw-e2e", 1u, 3u);
  if (rc != H2_PAL_OK) {
    fail_launcher("command_start", rc, false);
  }
  s_commands_started = true;
#endif



  rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta,
                                      H2_PAL_WIFI_POWER_SAVE_NONE);
  printf("H2_GIZCLAW_E2E_AMOLED stage=power_save mode=%d rc=%d\n",
         (int)H2_PAL_WIFI_POWER_SAVE_NONE, rc);
  fflush(stdout);
  if (rc != H2_PAL_OK) {
    fail_launcher("power_save", rc, s_commands_started);
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
    fail_launcher("wifi_supervisor", rc, s_commands_started);
  }
#if defined(H2_GIZCLAW_E2E_OTA_ONLY)
  /* Dedicated OTA source-image preparation keeps its existing lifecycle. */
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc != H2_PAL_OK) {
    fail_launcher("confirm", rc, s_commands_started);
  }
#endif
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
  bool network_fixture_checked = false;
  unsigned network_fixture_attempts = 0u;
  bool ble_advertising_paused = !s_commands_started;
  uint32_t ble_pause_retry_count = 0u;
  uint32_t time_retry_count = 0u;
  for (;;) {
    h2_runtime_event_t event = {
        .payload = payload.bytes,
        .payload_capacity = sizeof(payload.bytes),
    };
    /* Drain before waiting: a half-drained queue has no pending wake. */
    while (h2_runtime_poll_event(runtime, &event) == H2_PAL_OK) {
      wifi_has_ip = event_has_ip(event.kind, wifi_has_ip);
    }
    rc = h2_runtime_wait_notify(runtime, H2_GIZCLAW_E2E_AMOLED_EVENT_WAIT_MS);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT) {
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
            ESP_NETIF_SNTP_DEFAULT_CONFIG(h2_gizclaw_e2e_fixture_time_server());
        const esp_err_t time_init_rc = esp_netif_sntp_init(&sntp_config);
        if (time_init_rc != ESP_OK) {
          fail_launcher("time_init", (int)time_init_rc, s_commands_started);
        }
        sntp_initialized = true;
      }
      const esp_err_t time_rc = esp_netif_sntp_sync_wait(
          pdMS_TO_TICKS(H2_GIZCLAW_E2E_AMOLED_EVENT_WAIT_MS));
      if (time_rc == ESP_OK) {
        /* SNTP updates the SDK clock directly. Publish its calibrated value
         * through Runtime so the Time PAL validity gate also becomes ready. */
        struct timeval wall;
        if (gettimeofday(&wall, NULL) != 0 || wall.tv_sec <= 0) {
          fail_launcher("time_read", H2_PAL_ERR_IO, s_commands_started);
        }
        const uint64_t wall_ms = (uint64_t)wall.tv_sec * 1000u +
                                 (uint64_t)wall.tv_usec / 1000u;
        rc = h2_pal_time_set_wall_ms(runtime->time, wall_ms);
        if (rc != H2_PAL_OK) {
          fail_launcher("time_publish", rc, s_commands_started);
        }
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
    if (wifi_has_ip && state.clock_ready && !network_fixture_checked) {
      rc = network_fixture_ready(runtime);
      network_fixture_checked = rc == H2_PAL_OK;
      if (!network_fixture_checked && ++network_fixture_attempts >= 6u)
        fail_launcher("network_fixture", rc, s_commands_started);
    }
    if (ble_advertising_paused && network_fixture_checked &&
        h2_gizclaw_e2e_amoled_state_set_prerequisites(
            &state, wifi_has_ip, state.clock_ready)) {
      s_runner.runtime = runtime;
      s_runner.result = (h2_gizclaw_e2e_result_t){0};
      s_runner.exit_code = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
      s_runner.confirm_rc = H2_PAL_ERR_INVALID_STATE;
      char *evidence = h2_pal_mem_alloc(runtime->mem, H2_GIZCLAW_E2E_LEDGER_CAPACITY);
      const h2_pal_mutex_config_t mutex_config = {
          .name = "gizclaw-e2e-evidence", .allocator = runtime->mem,
      };
      uint8_t nonce[16];
      if (evidence == NULL ||
          h2_gizclaw_e2e_ledger_init(&s_runner.ledger, evidence, H2_GIZCLAW_E2E_LEDGER_CAPACITY) != H2_PAL_OK ||
          h2_atomic_init(&s_runner.capture_failed, false) != H2_ATOMIC_OK ||
          h2_pal_mutex_create(runtime->sync, &mutex_config, &s_runner.evidence_mutex) != H2_PAL_OK ||
          h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce)) != H2_PAL_OK)
        fail_launcher("evidence_init", H2_PAL_ERR_NO_MEMORY, s_commands_started);
      static const char hex[] = "0123456789abcdef";
      for (size_t i = 0u; i < sizeof(nonce); ++i) {
        s_runner.execution[i * 2u] = hex[nonce[i] >> 4u];
        s_runner.execution[i * 2u + 1u] = hex[nonce[i] & 15u];
      }
      s_runner.execution[32] = '\0';
      if (h2_atomic_init(&s_runner.exited, false) != H2_ATOMIC_OK)
        fail_launcher("runner_atomic_init", H2_PAL_ERR_NO_MEMORY, s_commands_started);
      const h2_pal_task_options_t runner_options = {
          .name = h2_gizclaw_e2e_launcher_task_name,
          .min_stack_size = H2_GIZCLAW_E2E_AMOLED_RUNNER_STACK_SIZE,
      };
      rc = h2_pal_task_start(runtime->task, &runner_options, run_e2e,
                             &s_runner, &runner_task);
      if (rc != H2_PAL_OK) {
        fail_launcher("runner_start", rc, s_commands_started);
      }
      printf("H2_GIZCLAW_E2E_AMOLED stage=runner status=STARTED\n");
      fflush(stdout);
    }

    uint64_t now_ms = 0u;
    if (runner_task != NULL && !state.runner_complete &&
        h2_atomic_load_explicit(&s_runner.exited, H2_ATOMIC_ACQUIRE)) {
      rc = h2_pal_task_join(runtime->task, runner_task);
      if (rc != H2_PAL_OK) {
        fail_launcher("runner_join", rc, s_commands_started);
      }
      runner_task = NULL;
      h2_atomic_destroy(&s_runner.exited);
      rc = h2_esp_h2loader_app_commands_start(runtime, "gizclaw-e2e", 1u, 3u);
      if (rc != H2_PAL_OK)
        fail_launcher("command_start", rc, false);
      s_commands_started = true;
      (void)h2_esp_h2loader_app_commands_pause_ble_advertising();
      if (s_runner.result.retained_resources == 0u &&
          !h2_atomic_load_explicit(&s_runner.capture_failed, H2_ATOMIC_ACQUIRE)) {
        char summary[1024];
        int size = format_summary(&s_runner, false, summary, sizeof(summary));
        if (size > 0 && (size_t)size < sizeof(summary))
          capture_evidence(&s_runner, summary, (size_t)size);
        else
          h2_atomic_store_explicit(&s_runner.capture_failed, true, H2_ATOMIC_RELEASE);
        if (!h2_atomic_load_explicit(&s_runner.capture_failed, H2_ATOMIC_ACQUIRE) &&
            h2_gizclaw_e2e_ledger_freeze(&s_runner.ledger) == H2_PAL_OK &&
            s_runner.exit_code == H2_GIZCLAW_E2E_EXIT_PASS &&
            h2_gizclaw_e2e_result_all_passed(&s_runner.result)) {
          int confirm = h2_esp_h2loader_app_confirm(runtime);
          s_runner.confirm_rc = confirm;
          printf("H2_GIZCLAW_CONFIRM rc=%d\n", confirm);
          s_runner.admitted = confirm == H2_PAL_OK;
          if (!s_runner.admitted)
            s_runner.exit_code = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
        }
      }
      if (!s_runner.admitted && s_runner.exit_code == H2_GIZCLAW_E2E_EXIT_PASS)
        s_runner.exit_code = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
      if (h2_pal_time_get_monotonic_ms(runtime->time, &now_ms) != H2_PAL_OK) {
        fail_launcher("summary_clock", H2_PAL_ERR_UNAVAILABLE, s_commands_started);
      }
      h2_gizclaw_e2e_amoled_state_complete(
          &state, now_ms, config->summary_replay_interval_ms);
      emit_summary(&s_runner, false);
      replay_ledger(&s_runner);
    }
    if (state.runner_complete &&
        h2_pal_time_get_monotonic_ms(runtime->time, &now_ms) == H2_PAL_OK &&
        h2_gizclaw_e2e_amoled_state_take_summary_replay(
            &state, now_ms, config->summary_replay_interval_ms)) {
      emit_summary(&s_runner, true);
      replay_ledger(&s_runner);
    }
  }
}

static void log_shutdown(void) {
  /* ROM output survives the command-transport teardown. Addresses identify
   * the actual orderly reset caller without exposing request or audio data. */
  esp_rom_printf("H2_GIZCLAW_SHUTDOWN uptime_us=%llu\n",
                 (unsigned long long)esp_timer_get_time());
  (void)esp_backtrace_print(8);
}

void app_main(void) {
  printf("H2_GIZCLAW_BOOT platform=amoled reset_reason=%d\n",
         (int)esp_reset_reason());
  fflush(stdout);
  if (esp_register_shutdown_handler(log_shutdown) != ESP_OK) {
    printf("H2_GIZCLAW_SETUP_FAIL stage=shutdown_observer\n");
    return;
  }
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
  h2_pal_result_t rc = h2_esp_board_start_entry_task(
      "amoled/gizclaw-e2e", image_entry, NULL);
  if (rc != H2_PAL_OK) {
    printf("H2_BOARD_ENTRY_FAIL board=amoled image=gizclaw-e2e code=%d\n",
           rc);
  }
}
