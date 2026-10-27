/* Target-owned orchestration; portable server, UI and network assertions stay
 * in the existing App. The peer runs the original 30/30/60 client matrices. */
#include "h2_iperf_server_app.h"
#include <stdio.h>
#include <string.h>

static h2_runtime_t* runtime;
static h2_iperf_server_app_t* app;
static const h2_iperf_server_app_mode_t modes[] = {
    H2_IPERF_SERVER_APP_MODE_IPV4, H2_IPERF_SERVER_APP_MODE_IPV6,
    H2_IPERF_SERVER_APP_MODE_DUAL};
static const char* const ids[] = {"ui-ready", "ipv4-matrix", "ipv6-matrix",
                                  "dual-stack-matrix", "shutdown"};
static unsigned phase_index, statuses[5];
static int first_error;
static bool phase_ready, packets_complete, stop_requested;
static uint64_t phase_started, completed_at, stop_started;
#ifndef H2_MOSAICO_SERVER_NOW
#define H2_MOSAICO_SERVER_NOW(out) \
  h2_pal_time_get_monotonic_ms(runtime->time, out)
#endif
static void record(unsigned index, int rc, uint32_t ipv4, uint32_t ipv6) {
  if (statuses[index]) return;
  statuses[index] = rc ? 2u : 1u;
  printf(
      "H2_IPERF_SERVER_E2E_CASE {\"id\":\"%s\",\"status\":\"%s\","
      "\"rc\":%d,\"ipv4_measurements\":%lu,\"ipv6_measurements\":%lu}\n",
      ids[index], rc ? "FAIL" : "PASS", rc, (unsigned long)ipv4,
      (unsigned long)ipv6);
  fflush(stdout);
}
static bool stop_with_error(int rc) {
  if (!first_error) first_error = rc;
  if (phase_index < 3) record(phase_index + 1, rc, 0, 0);
  return true;
}
static int ready(void* user) {
  (void)user;
  record(0, H2_PAL_OK, 0, 0);
  int rc = H2_MOSAICO_SERVER_NOW(&phase_started);
  if (!rc) rc = h2_iperf_server_app_request(app, modes[0], true);
  if (rc) (void)stop_with_error(rc);
  return rc;
}
static bool should_exit(void* user) {
  (void)user;
  if (first_error || phase_index == 3) return true;
  uint64_t now;
  int rc = H2_MOSAICO_SERVER_NOW(&now);
  if (rc) return stop_with_error(rc);
  if (now - phase_started >= 650000u)
    return stop_with_error(H2_PAL_ERR_TIMEOUT);
  h2_iperf_server_app_snapshot_t snapshot;
  rc = h2_iperf_server_app_snapshot(app, &snapshot);
  if (rc) return stop_with_error(rc);
  if (snapshot.phase == H2_IPERF_SERVER_APP_ERROR ||
      (snapshot.phase == H2_IPERF_SERVER_APP_STOPPING && snapshot.error))
    return stop_with_error(snapshot.error ? snapshot.error : H2_PAL_ERR_IO);
  if (stop_requested) {
    if (snapshot.phase != H2_IPERF_SERVER_APP_STOPPED) {
      if (now - stop_started >= 20000u)
        return stop_with_error(H2_PAL_ERR_TIMEOUT);
      return false;
    }
    record(phase_index + 1, H2_PAL_OK, snapshot.streams[0].completed_tests,
           snapshot.streams[1].completed_tests);
    ++phase_index;
    if (phase_index == 3) return true;
    phase_ready = packets_complete = stop_requested = false;
    phase_started = now;
    rc = h2_iperf_server_app_request(app, modes[phase_index], true);
    return rc ? stop_with_error(rc) : false;
  }
  if (snapshot.phase != H2_IPERF_SERVER_APP_LISTENING ||
      snapshot.mode != modes[phase_index])
    return false;
  if (!phase_ready) {
    phase_ready = true;
    phase_started = now;
    printf("H2_MOSAICO_IPERF_SERVER_READY mode=%u\n",
           (unsigned)modes[phase_index]);
    fflush(stdout);
  }
  bool enough4 = modes[phase_index] == H2_IPERF_SERVER_APP_MODE_IPV6 ||
                 snapshot.streams[0].completed_tests >= 30;
  bool enough6 = modes[phase_index] == H2_IPERF_SERVER_APP_MODE_IPV4 ||
                 snapshot.streams[1].completed_tests >= 30;
  if (enough4 && enough6 && !snapshot.streams[0].active &&
      !snapshot.streams[1].active && !packets_complete) {
    packets_complete = true;
    completed_at = now;
  }
  /* Leave the original peer's bounded disconnect/reconnect and saved-config
   * checks time to finish before changing its AP. No touch events are faked. */
  if (packets_complete && now - completed_at >= 60000u) {
    rc = h2_iperf_server_app_request(app, modes[phase_index], false);
    if (rc) return stop_with_error(rc);
    stop_requested = true;
    stop_started = now;
  }
  return false;
}
#ifndef H2_MOSAICO_SERVER_HOST_TEST
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mosaico_usb_console.h"
#include "h2_mosaico_loader_usb.h"
#include "iperf_ap.h"
static h2_iperf_amoled_ap_t ap;
static void hold(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char* stage, int rc) {
  if (!statuses[0]) record(0, rc, 0, 0);
  printf("H2_MOSAICO_IPERF_SERVER_SETUP stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static void run(void* user) {
  (void)user;
  ap.runtime = runtime;
  const h2_iperf_server_app_config_t config = {
      .network_user = &ap,
      .network_start = h2_iperf_amoled_ap_start,
      .network_stop = h2_iperf_amoled_ap_stop,
      .ui_ready = ready};
  int rc = h2_iperf_server_app_create(runtime, &config, &app);
  if (rc) fail("controller", rc);
  rc = h2_iperf_server_app_run_ui(app, should_exit, NULL);
  if (!statuses[0]) record(0, rc ? rc : H2_PAL_ERR_IO, 0, 0);
  if (rc && phase_index < 3) (void)stop_with_error(rc);
  const int cleanup = h2_iperf_server_app_destroy(&app);
  record(4, cleanup ? cleanup : rc, 0, 0);
  if (!rc) rc = first_error;
  if (!rc) rc = cleanup;
  for (unsigned i = 0; i < 5; ++i)
    if (statuses[i] != 1 && !rc) rc = H2_PAL_ERR_INVALID_STATE;
  const int confirm =
      rc ? H2_PAL_ERR_INVALID_STATE : h2_esp_h2loader_app_confirm(runtime);
  printf("H2_MOSAICO_IPERF_SERVER_DONE rc=%d confirm=%d\n", rc, confirm);
  fflush(stdout);
  hold();
}
void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc) fail("task-policy", rc);
  rc = h2_mosaico_usb_console_init();
  if (rc) fail("usb-console", rc);
  rc = h2_mosaico_loader_usb_init();
  if (rc) fail("usb-transport", rc);
  h2_runtime_config_t config = {0};
  rc = h2_esp_board_runtime_config(&config);
  if (rc) fail("board", rc);
  config.mem = h2_esp_platform_psram_allocator();
  const h2_esp_h2loader_app_commands_config_t commands = {
      .active_name = "iperf-server",
      .hardware_capabilities =
          H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI,
      .h2loader_partition_id = 1,
      .coredump_partition_id = 3};
  rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config,
                                                               &commands);
  if (rc) fail("commands", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc) fail("runtime", rc);
  rc = h2_esp_platform_wifi_ensure_started();
  if (rc) fail("wifi-init", rc);
  printf("H2_MOSAICO_IPERF_SERVER_BOOT version=%s board=esp_mosaico\n",
         esp_app_get_description()->version);
  fflush(stdout);
  const h2_pal_task_options_t options = {.name = "mosaico/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc) fail("worker", rc);
}
#endif
