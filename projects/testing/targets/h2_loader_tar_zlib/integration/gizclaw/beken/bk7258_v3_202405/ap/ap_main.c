#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_corehttp.h"
#include "h2_device_service.h"
#include "h2_gizclaw_e2e_execution.h"
#include "h2_gizclaw_e2e_task_names.h"
#include <os/os.h>
#include <components/system.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char h2_gizclaw_e2e_pcm[];
extern const size_t h2_gizclaw_e2e_pcm_size;
static h2_device_app_commands_t h2_management = {
    .config =
        {
            .active_name = "app",
            .hardware_capabilities =
                H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE,
            .h2loader_partition_id = 1u,
            .app_partition_id = 2u,
            .coredump_partition_id = 0x7a0000u,
            .app_entry_path = "app/bk/app_ab_crc.rbl",
            .boot_policy_flags = H2_DEVICE_BOOT_PREPARE_SELECTION |
                                 H2_DEVICE_BOOT_PREPARE_APP_RESTART,
            .identity_policy = H2_DEVICE_IDENTITY_PADDED_WINDOW,
            .serial_post_write_poll_ms = 1u,
            .command_stack_size = 32768u,
            .ble_start_attempts = 1u,
            .ble_advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
        },
};

/* This image keeps Runtime, providers and App capture alive for recovery and
 * repeated evidence reads, including when the App reports retained resources.
 */
static h2_runtime_t* runtime;
static h2_gizclaw_e2e_execution_t* execution;
static h2_corehttp_t* fixture_http;
static h2_pal_http_api_t fixture_http_api;

static void hold(void) {
  for (;;) rtos_delay_milliseconds(1000u);
}

static void fail(const char* stage, int rc) {
  printf("H2_GIZCLAW_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  /* CP consumed the one-shot trial boot request. A setup failure returns to
   * the preserved Loader without admitting an unqualified App image. */
  rtos_delay_milliseconds(250u);
  bk_reboot();
  hold();
}

static int network_ready(void) {
  h2_pal_wifi_sta_status_t status = {0};
  int rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
  if (!rc && status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
      status.ip.ip4)
    return H2_PAL_OK;
  h2_pal_wifi_sta_config_t saved = {0};
  rc =
      h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &saved);
  if (!rc) rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &saved, 20000u);
  memset(&saved, 0, sizeof(saved));
  if (rc) return rc;
  for (unsigned attempt = 0u; attempt < 200u; ++attempt) {
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (rc) return rc;
    if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
        status.ip.ip4)
      return H2_PAL_OK;
    rtos_delay_milliseconds(100u);
  }
  return H2_PAL_ERR_TIMEOUT;
}

static int create_fixture_http(h2_runtime_config_t* config) {
  const char* hex = h2_gizclaw_e2e_fixture_root_ca_hex();
  const size_t size = strlen(hex);
  if (!size || size % 2u || size > 32768u) return H2_PAL_ERR_INVALID_ARG;
  uint8_t* pem = h2_pal_mem_alloc(config->mem, size / 2u);
  if (!pem) return H2_PAL_ERR_NO_MEMORY;
  for (size_t i = 0u; i < size; i += 2u) {
    const unsigned high = hex[i] <= '9' ? (unsigned)(hex[i] - '0')
                                        : (unsigned)(hex[i] - 'a') + 10u;
    const unsigned low = hex[i + 1u] <= '9'
                             ? (unsigned)(hex[i + 1u] - '0')
                             : (unsigned)(hex[i + 1u] - 'a') + 10u;
    if (high > 15u || low > 15u) {
      h2_pal_mem_free(config->mem, pem);
      return H2_PAL_ERR_INVALID_ARG;
    }
    pem[i / 2u] = (uint8_t)((high << 4u) | low);
  }
  const h2_corehttp_config_t http = {
      .allocator = config->mem,
      .net = config->net,
      .time = config->time,
      .log = config->log,
      .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
      .root_ca_pem = pem,
      .root_ca_pem_len = size / 2u};
  const int rc = h2_corehttp_create(&http, &fixture_http, &fixture_http_api);
  h2_pal_mem_free(config->mem, pem);
  if (rc == H2_PAL_OK) config->http = &fixture_http_api;
  return rc;
}

static void run(void* user) {
  (void)user;
  if (!h2_gizclaw_e2e_fixture_key()[0] ||
      !h2_gizclaw_e2e_fixture_profile()[0] ||
      !h2_gizclaw_e2e_fixture_value()[0] ||
      !h2_gizclaw_e2e_fixture_endpoint()[0] ||
      !h2_gizclaw_e2e_fixture_token()[0] ||
      !h2_gizclaw_e2e_fixture_device_api_url()[0] ||
      !h2_gizclaw_e2e_fixture_audio_url()[0]) {
    printf("H2_GIZCLAW_SETUP_FAIL stage=missing_app_config_fixture rc=%d\n",
           H2_PAL_ERR_INVALID_ARG);
    fflush(stdout);
    return;
  }
  int rc;
  while ((rc = network_ready()) != H2_PAL_OK) {
    printf("H2_GIZCLAW_SETUP_WAIT stage=network rc=%d\n", rc);
    fflush(stdout);
    rtos_delay_milliseconds(3000u);
  }
  const char* endpoint = h2_gizclaw_e2e_fixture_endpoint();
  const char* token = h2_gizclaw_e2e_fixture_token();
  const h2_gizclaw_e2e_config_t app = {
      .server_endpoint = {endpoint, strlen(endpoint)},
      .registration_token = {token, strlen(token)},
      .app_config_key = h2_gizclaw_e2e_fixture_key(),
      .expected_runtime_profile = h2_gizclaw_e2e_fixture_profile(),
      .workflow_tag = h2_gizclaw_e2e_fixture_workflow_tag(),
      .app_config_expected_value = {h2_gizclaw_e2e_fixture_value(),
                                    strlen(h2_gizclaw_e2e_fixture_value())},
      .voice_audio =
          h2_gizclaw_e2e_fixture_physical_audio() ? runtime->audio : NULL,
      .voice_pcm_s16le_16khz_mono = h2_gizclaw_e2e_pcm,
      .voice_pcm_len = h2_gizclaw_e2e_pcm_size,
      .device_api_url = h2_gizclaw_e2e_fixture_device_api_url(),
      .device_audio_url = h2_gizclaw_e2e_fixture_audio_url(),
      .device_real_audio = h2_gizclaw_e2e_fixture_physical_audio() != 0,
      .suites = H2_GIZCLAW_E2E_SUITE_ALL};
  const h2_gizclaw_e2e_execution_config_t evidence = {
      .output = h2_bk_platform_console_log_api(),
      .board = "bk7258",
      .platform = "bk7258",
      .physical_audio = h2_gizclaw_e2e_fixture_physical_audio() != 0};
  rc = h2_gizclaw_e2e_execution_create(runtime, &app, &evidence, &execution);
  if (rc) fail("evidence", rc);
  int confirm_rc = H2_PAL_ERR_INVALID_STATE;
  if (h2_gizclaw_e2e_execution_run(execution) == H2_GIZCLAW_E2E_EXIT_PASS &&
      h2_gizclaw_e2e_execution_qualified(execution))
    confirm_rc = h2_device_app_confirm(&h2_management, runtime);
  rc = h2_gizclaw_e2e_execution_publish(execution, confirm_rc);
  if (rc) fail("evidence_publish", rc);
  for (;;) {
    rc = h2_gizclaw_e2e_execution_replay(execution);
    if (rc) printf("H2_GIZCLAW_SETUP_FAIL stage=evidence_replay rc=%d\n", rc);
    rtos_delay_milliseconds(10000u);
  }
}

static void entry(void* user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc) fail("board", rc);
  /* Fixture and ledger allocations use PSRAM, preserving SDK networking SRAM.
   */
  config.mem = h2_bk7258_board_psram_allocator();
  /* This board has no system CA bundle. The target owns the verified provider
   * and its trust fixture for the full recovery lifetime. */
  rc = create_fixture_http(&config);
  if (rc) {
    h2_bk7258_board_runtime_deinit();
    fail("http_trust", rc);
  }
  rc = h2_runtime_init(&config, &runtime);
  if (rc) {
    h2_corehttp_destroy(fixture_http);
    fixture_http = NULL;
    h2_bk7258_board_runtime_deinit();
    fail("runtime", rc);
  }
  rc = h2_device_app_commands_start_serial_named(
      &h2_management, runtime, "gizclaw-e2e",
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
  if (rc) fail("commands", rc);
  h2_pal_task_t* runner = NULL;
  const h2_pal_task_options_t options = {.name =
                                             h2_gizclaw_e2e_launcher_task_name};
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc) fail("runner", rc);
  hold();
}

int main(void) {
  const int policy_rc = h2_bk_target_task_policy_install();
  if (policy_rc) return -1;
  bk_init();
  const int rc =
      h2_bk7258_board_start_entry_task("bk/gizclaw-e2e", entry, NULL);
  if (rc) fail("entry", rc);
  return 0;
}
