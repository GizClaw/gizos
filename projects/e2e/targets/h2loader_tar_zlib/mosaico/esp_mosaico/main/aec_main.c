#include "lwip/sockets.h"
/* Mosaico composition for the existing AEC App and a separate acoustic source.
 */
#include "h2_aec_calibration.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_target_task_policy.h"
#include "h2_esp_platform_core.h"
#include "h2_mosaico_usb_console.h"
#include "h2_mosaico_loader_usb.h"
#include "h2_e2e_fixture_wifi.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#ifndef H2_AEC_SOURCE_URL
#error Explicit independent source URL required
#endif
#ifndef H2_AEC_SOURCE_TOKEN
#error Explicit independent source token required
#endif
static h2_runtime_t* runtime;
static esp_timer_handle_t watchdog;
static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char* stage, int rc) {
  printf("AEC_CALIBRATION_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void deadline(void* user) {
  (void)user;
  esp_restart();
}
static esp_err_t source_event(esp_http_client_event_t* event) {
  if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
    const int nodelay = 1;
    int* socket_rc = event->user_data;
    *socket_rc =
        setsockopt(esp_http_client_get_socket(event->client), IPPROTO_TCP,
                   TCP_NODELAY, &nodelay, sizeof(nodelay));
  }
  return ESP_OK;
}
static h2_pal_result_t source(void* user, bool enabled,
                              const h2_audio_pcm_format_t* f,
                              const int16_t* pcm, size_t samples,
                              uint32_t timeout) {
  (void)user;
  if (enabled && (f->sample_rate_hz != 16000 || f->channels != 1 ||
                  f->sample_format != H2_AUDIO_SAMPLE_S16LE || samples != 512 ||
                  pcm == NULL))
    return H2_PAL_ERR_UNSUPPORTED;
  char url[192];
  int n = snprintf(url, sizeof(url), "%s/%s", H2_AEC_SOURCE_URL,
                   enabled ? "probe" : "stop");
  if (n < 0 || (size_t)n >= sizeof(url)) return H2_PAL_ERR_INVALID_ARG;
  int socket_rc = -1;
  const esp_http_client_config_t cfg = {.url = url,
                                        .event_handler = source_event,
                                        .user_data = &socket_rc,
                                        .timeout_ms = (int)timeout,
                                        .disable_auto_redirect = true,
                                        .buffer_size = 512,
                                        .buffer_size_tx = 1536};
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return H2_PAL_ERR_NO_MEMORY;
  esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_POST);
  if (err == ESP_OK)
    err =
        esp_http_client_set_header(client, "X-AEC-Token", H2_AEC_SOURCE_TOKEN);
  if (err == ESP_OK)
    err = esp_http_client_set_header(client, "Content-Type",
                                     "application/octet-stream");
  if (err == ESP_OK)
    err = esp_http_client_set_post_field(
        client, enabled ? (const char*)pcm : "",
        enabled ? (int)(samples * sizeof(int16_t)) : 0);
  if (err == ESP_OK) err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  int rc = err == ESP_OK && status == 200 && socket_rc == 0 ? H2_PAL_OK
                                                            : H2_PAL_ERR_IO;
  printf("AEC_SOURCE_ACK enabled=%d rc=%d http_status=%d\n", enabled, rc,
         status);
  fflush(stdout);
  return rc;
}
static h2_pal_result_t emit(void* unused, const char* line, size_t length) {
  (void)unused;
  if (fwrite(line, 1, length, stdout) != length || putchar('\n') == EOF ||
      fflush(stdout))
    return H2_PAL_ERR_IO;
  return H2_PAL_OK;
}
static void run(void* unused) {
  (void)unused;
  const h2_pal_wifi_sta_config_t* wifi = h2_e2e_fixture_wifi();
  int rc = wifi ? h2_pal_wifi_sta_connect(runtime->wifi_sta, wifi, 20000u)
                : h2_runtime_wifi_connect_saved(runtime, 20000u);
  if (rc) fail("wifi", rc);
  rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta,
                                      H2_PAL_WIFI_POWER_SAVE_NONE);
  if (rc) fail("wifi-power-save", rc);
  h2_pal_wifi_sta_status_t wifi_status = {0};
  const int64_t ip_deadline = esp_timer_get_time() + 20000000;
  do {
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &wifi_status);
    if (!rc && wifi_status.ip_valid) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  } while (esp_timer_get_time() < ip_deadline);
  if (rc || !wifi_status.ip_valid)
    fail("wifi-ip", rc ? rc : H2_PAL_ERR_TIMEOUT);
  printf("AEC_CALIBRATION_NETWORK ipv4_ready=1\n");
  fflush(stdout);
  rc = source(NULL, false, NULL, NULL, 0u, 5000u);
  if (rc) fail("source-preflight", rc);
  const h2_aec_calibration_pair_t pairs[] = {{25u, 20u}, {50u, 40u}};
  const h2_aec_calibration_config_t config = {
      .audio = runtime->audio,
      .mem = runtime->mem,
      .time = runtime->time,
      .candidates = pairs,
      .candidate_count = 2,
      .selection = H2_AEC_CALIBRATION_PARETO_ONLY,
      .limits = h2_aec_calibration_default_limits(),
      .near_source = source};
  h2_aec_calibration_t* runner = NULL;
  const h2_aec_calibration_result_t* result = NULL;
  rc = h2_aec_calibration_create(&config, &runner);
  if (rc) fail("create", rc);
  uint64_t run_id = (uint64_t)esp_timer_get_time();
  printf(
      "AEC_CALIBRATION_BEGIN board=esp_mosaico version=%s source=amoled candidates=2\n",
      esp_app_get_description()->version);
  fflush(stdout);
  rc = h2_aec_calibration_run(runner, &result);
  int report = result ? h2_aec_calibration_report(result, run_id, emit, NULL)
                      : H2_PAL_ERR_INVALID_STATE;
  int cleanup = h2_aec_calibration_destroy(runner);
  if (!cleanup) runner = NULL;
  printf("AEC_CALIBRATION_TERMINAL rc=%d report=%d cleanup=%d\n", rc, report,
         cleanup);
  fflush(stdout);
  esp_timer_stop(watchdog);
  esp_timer_delete(watchdog);
  if (cleanup) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }
  int confirm = (!rc && !report) ? h2_esp_h2loader_app_confirm(runtime)
                                 : H2_PAL_ERR_INVALID_STATE;
  printf("H2_MOSAICO_E2E_DONE rc=%d report=%d cleanup=%d confirm=%d\n", rc,
         report, cleanup, confirm);
  fflush(stdout);
  hold();
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc) fail("policy", rc);
  rc = h2_mosaico_usb_console_init();
  if (rc) fail("console", rc);
  rc = h2_mosaico_loader_usb_init();
  if (rc) fail("usb", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc) fail("board", rc);
  config.mem = h2_esp_platform_psram_allocator();
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "aec-calibration",
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .h2loader_partition_id = 1,
      .coredump_partition_id = 3};
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                               &commands);
  if (rc) fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  const esp_timer_create_args_t timer = {.callback = deadline,
                                         .name = "aec-deadline"};
  if (esp_timer_create(&timer, &watchdog) != ESP_OK ||
      esp_timer_start_once(watchdog, 600000000) != ESP_OK)
    fail("timer", H2_PAL_ERR_IO);
  const h2_pal_task_options_t options = {.name = "mosaico/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc) fail("runner", rc);
}
