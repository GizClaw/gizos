#include "h2_h2loader_e2e_runner.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fake_executor {
  size_t count;
  h2_h2loader_e2e_case_t cases[40];
  h2_h2loader_e2e_transport_t transports[40];
} fake_executor_t;

static h2_pal_result_t execute_case(void *user,
                                    h2_h2loader_e2e_transport_t transport,
                                    h2_h2loader_e2e_case_t test_case,
                                    h2_h2loader_e2e_case_result_t *out_result) {
  fake_executor_t *fake = user;
  fake->cases[fake->count] = test_case;
  fake->transports[fake->count] = transport;
  ++fake->count;
  out_result->terminal = H2_H2LOADER_HOST_COMMAND_TERMINAL_OK;
  if (test_case == H2_H2LOADER_E2E_CASE_STATUS) {
    out_result->status_valid = 1u;
    out_result->status.command_availability =
        H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL;
  }
  if (test_case == H2_H2LOADER_E2E_CASE_MONITOR ||
      test_case == H2_H2LOADER_E2E_CASE_REBOOT_LOADER_MONITOR ||
      test_case == H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR ||
      test_case == H2_H2LOADER_E2E_CASE_REBOOT_UPGRADE_MONITOR) {
    out_result->log_bytes = 32u;
  }
  if (test_case == H2_H2LOADER_E2E_CASE_REBOOT_LOADER_MONITOR ||
      test_case == H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR) {
    out_result->reconnect_attempts = 1u;
  }
  return H2_PAL_OK;
}

static void test_full_sequence_for_both_transports(void) {
  fake_executor_t fake = {0};
  h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .ble_endpoint = "4:001122334455",
      .app_firmware = (const uint8_t *)"x",
      .app_firmware_size = 1u,
      .loader_firmware = (const uint8_t *)"y",
      .loader_firmware_size = 1u,
      .firmware_url = "http://example.test/update.tar.zlib",
      .firmware_url_bytes = 1u,
      .firmware_url_sha256 =
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      .wifi_ssid = "test",
      .wifi_password = "secret",
      .repeat_count = 1u,
      .include_wifi = 1u,
      .include_send = 1u,
      .include_send_url = 1u,
      .include_lifecycle = 1u,
      .expected_coredump_bytes = 16384u,
      .include_coredump = 1u,
      .execute_case = execute_case,
      .execute_user = &fake,
  };
  h2_pal_result_t rc = h2_h2loader_e2e_run(&config, &result);
  if (rc != H2_PAL_OK) {
    fprintf(stderr, "unexpected rc=%d cases=%zu passed=%zu failed=%zu\n", rc,
            result.case_count, result.passed, result.failed);
  }
  assert(rc == H2_PAL_OK);
  assert(result.complete == 1);
  assert(result.case_count == 40u);
  assert(result.passed == 40u);
  assert(result.failed == 0u);
  assert(fake.count == 40u);
  for (size_t i = 0u; i < 16u; ++i) {
    assert(fake.transports[i] == H2_H2LOADER_E2E_TRANSPORT_UART);
    assert(fake.transports[i + 16u] == H2_H2LOADER_E2E_TRANSPORT_BLE);
    assert(fake.cases[i] == fake.cases[i + 16u]);
  }
  assert(fake.cases[32] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS);
  assert(fake.cases[33] == H2_H2LOADER_E2E_CASE_COREDUMP_DUMP);
  assert(fake.cases[34] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS);
  assert(fake.cases[35] == H2_H2LOADER_E2E_CASE_COREDUMP_DUMP);
  assert(fake.cases[36] == H2_H2LOADER_E2E_CASE_COREDUMP_ERASE);
  assert(fake.cases[37] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE);
  assert(fake.cases[38] == H2_H2LOADER_E2E_CASE_COREDUMP_ERASE);
  assert(fake.cases[39] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE);
}

static h2_pal_result_t fail_send(void *user,
                                 h2_h2loader_e2e_transport_t transport,
                                 h2_h2loader_e2e_case_t test_case,
                                 h2_h2loader_e2e_case_result_t *out_result) {
  (void)user;
  (void)transport;
  if (test_case == H2_H2LOADER_E2E_CASE_STATUS) {
    out_result->status_valid = 1u;
    out_result->status.command_availability =
        H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL;
  }
  return test_case == H2_H2LOADER_E2E_CASE_SEND ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t execute_without_memory(
    void *user, h2_h2loader_e2e_transport_t transport,
    h2_h2loader_e2e_case_t test_case,
    h2_h2loader_e2e_case_result_t *out_result) {
  h2_pal_result_t rc = execute_case(user, transport, test_case, out_result);
  if (test_case == H2_H2LOADER_E2E_CASE_STATUS) {
    out_result->status.command_availability &=
        ~H2_H2LOADER_HOST_COMMAND_AVAILABLE_MEMORY;
  }
  return rc;
}

static h2_pal_result_t execute_with_removed_availability(
    void *user, h2_h2loader_e2e_transport_t transport,
    h2_h2loader_e2e_case_t test_case,
    h2_h2loader_e2e_case_result_t *out_result) {
  h2_pal_result_t rc = execute_case(user, transport, test_case, out_result);
  if (test_case == H2_H2LOADER_E2E_CASE_STATUS) {
    out_result->status.command_availability |= UINT32_C(1) << 6;
  }
  return rc;
}

static void test_memory_follows_authoritative_availability(void) {
  fake_executor_t fake = {0};
  h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .repeat_count = 1u,
      .execute_case = execute_without_memory,
      .execute_user = &fake,
  };
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_OK);
  assert(result.case_count == 4u);
  assert(fake.cases[0] == H2_H2LOADER_E2E_CASE_HELP);
  assert(fake.cases[1] == H2_H2LOADER_E2E_CASE_STATUS);
  assert(fake.cases[2] == H2_H2LOADER_E2E_CASE_STATS);
  assert(fake.cases[3] == H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT);
}

static void test_legacy_check_uses_preceding_status_availability(void) {
  fake_executor_t fake = {0};
  h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .repeat_count = 1u,
      .execute_case = execute_with_removed_availability,
      .execute_user = &fake,
  };
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_INVALID_STATE);
  assert(result.case_count == 5u);
  assert(result.cases[4].test_case ==
         H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT);
  assert(result.cases[4].result == H2_PAL_ERR_INVALID_STATE);
}

static void test_failure_is_reported_without_hiding_cleanup(void) {
  h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .app_firmware = (const uint8_t *)"x",
      .app_firmware_size = 1u,
      .repeat_count = 1u,
      .include_send = 1u,
      .execute_case = fail_send,
  };
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_IO);
  assert(result.case_count == 7u);
  assert(result.failed == 1u);
  assert(result.cases[6].test_case ==
         H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND);
  assert(result.cases[6].result == H2_PAL_OK);
}

static void test_invalid_configs(void) {
  h2_h2loader_e2e_result_t result;
  h2_h2loader_e2e_config_t config = {.repeat_count = 1u};
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_INVALID_ARG);
  config.uart_endpoint = "/dev/test";
  config.include_wifi = 1u;
  config.execute_case = execute_case;
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_INVALID_ARG);
  config.include_wifi = 0u;
  config.include_coredump = 1u;
  config.expected_coredump_bytes = 3u;
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_INVALID_ARG);
  config.include_coredump = 0u;
  config.include_monitor = 1u;
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_INVALID_ARG);
}

static void test_monitor_cases_are_uart_only_and_bounded(void) {
  fake_executor_t fake = {0};
  h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .ble_endpoint = "4:001122334455",
      .repeat_count = 1u,
      .monitor_duration_ms = 500u,
      .include_monitor = 1u,
      .execute_case = execute_case,
      .execute_user = &fake,
  };
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_OK);
  assert(result.case_count == 13u);
  assert(fake.cases[0] == H2_H2LOADER_E2E_CASE_HELP);
  assert(fake.cases[1] == H2_H2LOADER_E2E_CASE_STATUS);
  assert(fake.cases[2] == H2_H2LOADER_E2E_CASE_STATS);
  assert(fake.cases[3] == H2_H2LOADER_E2E_CASE_MEMORY);
  assert(fake.cases[4] == H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT);
  assert(fake.cases[5] == H2_H2LOADER_E2E_CASE_MONITOR);
  assert(fake.cases[6] == H2_H2LOADER_E2E_CASE_REBOOT_LOADER_MONITOR);
  assert(fake.cases[7] == H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR);
  assert(fake.cases[8] == H2_H2LOADER_E2E_CASE_HELP);
  assert(result.cases[5].log_bytes == 32u);
  assert(result.cases[5].reconnect_attempts == 0u);
  assert(result.cases[6].log_bytes == 32u);
  assert(result.cases[6].reconnect_attempts == 1u);
  assert(result.cases[7].log_bytes == 32u);
  assert(result.cases[7].reconnect_attempts == 1u);
  for (size_t i = 0u; i < 8u; ++i)
    assert(fake.transports[i] == H2_H2LOADER_E2E_TRANSPORT_UART);
  for (size_t i = 8u; i < 13u; ++i)
    assert(fake.transports[i] == H2_H2LOADER_E2E_TRANSPORT_BLE);
}

static void test_monitor_runs_each_reboot_with_a_bootable_target(void) {
  fake_executor_t fake = {0};
  h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .app_firmware = (const uint8_t *)"x",
      .app_firmware_size = 1u,
      .loader_firmware = (const uint8_t *)"y",
      .loader_firmware_size = 1u,
      .repeat_count = 1u,
      .monitor_duration_ms = 500u,
      .include_lifecycle = 1u,
      .include_monitor = 1u,
      .execute_case = execute_case,
      .execute_user = &fake,
  };
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_OK);
  assert(result.case_count == 12u);
  assert(fake.cases[0] == H2_H2LOADER_E2E_CASE_HELP);
  assert(fake.cases[1] == H2_H2LOADER_E2E_CASE_STATUS);
  assert(fake.cases[2] == H2_H2LOADER_E2E_CASE_STATS);
  assert(fake.cases[3] == H2_H2LOADER_E2E_CASE_MEMORY);
  assert(fake.cases[4] == H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT);
  assert(fake.cases[5] == H2_H2LOADER_E2E_CASE_MONITOR);
  assert(fake.cases[6] == H2_H2LOADER_E2E_CASE_REBOOT_LOADER_MONITOR);
  assert(fake.cases[7] == H2_H2LOADER_E2E_CASE_REBOOT_UPGRADE_MONITOR);
  assert(fake.cases[8] == H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR);
  assert(fake.cases[11] == H2_H2LOADER_E2E_CASE_INSTALL_LOADER);
}

static void test_names(void) {
  assert(strcmp(h2_h2loader_e2e_transport_name(H2_H2LOADER_E2E_TRANSPORT_UART),
                "uart") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_SEND_URL),
                "send-url") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_HELP),
                "help") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_MEMORY),
                "memory") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(
                    H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT),
                "legacy-commands-absent") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(
                    H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE),
                "coredump-status-after-erase") == 0);
  assert(
      strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR),
             "reboot-app-monitor") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(
                    H2_H2LOADER_E2E_CASE_REBOOT_UPGRADE_MONITOR),
                "reboot-upgrade-monitor") == 0);
}

int main(void) {
  test_full_sequence_for_both_transports();
  test_failure_is_reported_without_hiding_cleanup();
  test_memory_follows_authoritative_availability();
  test_legacy_check_uses_preceding_status_availability();
  test_invalid_configs();
  test_monitor_cases_are_uart_only_and_bounded();
  test_monitor_runs_each_reboot_with_a_bootable_target();
  test_names();
  return 0;
}
