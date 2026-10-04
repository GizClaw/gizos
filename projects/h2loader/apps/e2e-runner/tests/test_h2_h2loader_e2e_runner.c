#include "h2_h2loader_e2e_runner.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "h2_h2loader_host_package.h"
#include <string.h>

typedef struct fake_executor {
  size_t count;
  h2_h2loader_e2e_case_t cases[H2_H2LOADER_E2E_MAX_CASES];
  h2_h2loader_e2e_transport_t transports[H2_H2LOADER_E2E_MAX_CASES];
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
  if (test_case == H2_H2LOADER_E2E_CASE_STATUS ||
      test_case == H2_H2LOADER_E2E_CASE_APP_STATUS) {
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
  static h2_h2loader_e2e_result_t result;
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
  assert(result.case_count == 64u);
  assert(result.passed == 64u);
  assert(result.failed == 0u);
  assert(fake.count == 64u);
  for (size_t i = 0u; i < 28u; ++i) {
    assert(fake.transports[i] == H2_H2LOADER_E2E_TRANSPORT_UART);
    assert(fake.transports[i + 28u] == H2_H2LOADER_E2E_TRANSPORT_BLE);
    assert(fake.cases[i] == fake.cases[i + 28u]);
  }
  assert(fake.cases[13] == H2_H2LOADER_E2E_CASE_APP_HELP);
  assert(fake.cases[14] == H2_H2LOADER_E2E_CASE_APP_STATUS);
  assert(fake.cases[15] == H2_H2LOADER_E2E_CASE_APP_STATS);
  assert(fake.cases[16] == H2_H2LOADER_E2E_CASE_APP_MEMORY);
  assert(fake.cases[17] ==
         H2_H2LOADER_E2E_CASE_APP_LEGACY_COMMANDS_ABSENT);
  assert(fake.cases[18] == H2_H2LOADER_E2E_CASE_APP_WIFI_SCAN);
  assert(fake.cases[21] == H2_H2LOADER_E2E_CASE_APP_SEND);
  assert(fake.cases[22] == H2_H2LOADER_E2E_CASE_APP_STAGE_ABORT_AFTER_SEND);
  assert(fake.cases[23] == H2_H2LOADER_E2E_CASE_APP_SEND_URL);
  assert(fake.cases[24] ==
         H2_H2LOADER_E2E_CASE_APP_STAGE_ABORT_AFTER_SEND_URL);
  assert(fake.cases[56] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS);
  assert(fake.cases[57] == H2_H2LOADER_E2E_CASE_COREDUMP_DUMP);
  assert(fake.cases[58] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS);
  assert(fake.cases[59] == H2_H2LOADER_E2E_CASE_COREDUMP_DUMP);
  assert(fake.cases[60] == H2_H2LOADER_E2E_CASE_COREDUMP_ERASE);
  assert(fake.cases[61] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE);
  assert(fake.cases[62] == H2_H2LOADER_E2E_CASE_COREDUMP_ERASE);
  assert(fake.cases[63] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE);
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
  static h2_h2loader_e2e_result_t result;
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
  static h2_h2loader_e2e_result_t result;
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
  static h2_h2loader_e2e_result_t result;
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
  static h2_h2loader_e2e_result_t result;
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
  config.include_monitor = 0u;
  config.include_crash = 1u;
  config.crash_firmware = (const uint8_t *)"z";
  config.crash_firmware_size = 1u;
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_INVALID_ARG);
}

static void test_monitor_cases_are_uart_only_and_bounded(void) {
  fake_executor_t fake = {0};
  static h2_h2loader_e2e_result_t result;
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
  static h2_h2loader_e2e_result_t result;
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
  assert(result.case_count == 17u);
  assert(fake.cases[0] == H2_H2LOADER_E2E_CASE_HELP);
  assert(fake.cases[1] == H2_H2LOADER_E2E_CASE_STATUS);
  assert(fake.cases[2] == H2_H2LOADER_E2E_CASE_STATS);
  assert(fake.cases[3] == H2_H2LOADER_E2E_CASE_MEMORY);
  assert(fake.cases[4] == H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT);
  assert(fake.cases[5] == H2_H2LOADER_E2E_CASE_MONITOR);
  assert(fake.cases[6] == H2_H2LOADER_E2E_CASE_REBOOT_LOADER_MONITOR);
  assert(fake.cases[7] == H2_H2LOADER_E2E_CASE_REBOOT_UPGRADE_MONITOR);
  assert(fake.cases[8] == H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR);
  assert(fake.cases[9] == H2_H2LOADER_E2E_CASE_APP_HELP);
  assert(fake.cases[10] == H2_H2LOADER_E2E_CASE_APP_STATUS);
  assert(fake.cases[11] == H2_H2LOADER_E2E_CASE_APP_STATS);
  assert(fake.cases[12] == H2_H2LOADER_E2E_CASE_APP_MEMORY);
  assert(fake.cases[13] ==
         H2_H2LOADER_E2E_CASE_APP_LEGACY_COMMANDS_ABSENT);
  assert(fake.cases[16] == H2_H2LOADER_E2E_CASE_INSTALL_LOADER);
}

static void test_crash_app_runs_once_before_cross_transport_coredump(void) {
  fake_executor_t fake = {0};
  static h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .ble_endpoint = "4:001122334455",
      .crash_firmware = (const uint8_t *)"z",
      .crash_firmware_size = 1u,
      .repeat_count = 1u,
      .include_coredump = 1u,
      .include_crash = 1u,
      .execute_case = execute_case,
      .execute_user = &fake,
  };
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_OK);
  assert(result.case_count == 19u);
  assert(fake.cases[10] == H2_H2LOADER_E2E_CASE_INSTALL_CRASH_APP);
  assert(fake.transports[10] == H2_H2LOADER_E2E_TRANSPORT_UART);
  assert(fake.cases[11] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS);
  assert(fake.cases[12] == H2_H2LOADER_E2E_CASE_COREDUMP_DUMP);
  assert(fake.cases[13] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS);
  assert(fake.cases[14] == H2_H2LOADER_E2E_CASE_COREDUMP_DUMP);
  assert(fake.cases[15] == H2_H2LOADER_E2E_CASE_COREDUMP_ERASE);
  assert(fake.cases[18] == H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE);
}

static int cancel_after_first_case(void *user) {
  return ((fake_executor_t *)user)->count != 0u;
}

static void test_cancel_does_not_execute_later_cases(void) {
  fake_executor_t fake = {0};
  static h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .ble_endpoint = "4:001122334455",
      .repeat_count = 1u,
      .execute_case = execute_case,
      .execute_user = &fake,
      .is_cancelled = cancel_after_first_case,
      .cancel_user = &fake,
  };
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_EXIT);
  assert(fake.count == 1u);
  assert(result.passed == 1u);
  for (size_t i = 1u; i < result.case_count; ++i) {
    assert(result.cases[i].result == H2_PAL_EXIT);
  }
}

static void test_names(void) {
  assert(strcmp(h2_h2loader_e2e_transport_name(H2_H2LOADER_E2E_TRANSPORT_UART),
                "uart") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_SEND_URL),
                "send-url") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_APP_SEND_URL),
                "app-send-url") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_APP_STATUS),
                "app-status") == 0);
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
  assert(
      strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_INSTALL_CRASH_APP),
             "install-crash-app") == 0);
}

static void *matrix_alloc(void *user, size_t len) { (void)user; return malloc(len); }
static void *matrix_realloc(void *user, void *data, size_t len) { (void)user; return realloc(data, len); }
static void matrix_free(void *user, void *data) { (void)user; free(data); }
static const h2_pal_mem_vtable_t matrix_mem_vtable = {
    .alloc = matrix_alloc, .realloc = matrix_realloc, .free = matrix_free};
static const h2_pal_mem_api_t matrix_mem = {.vtable = &matrix_mem_vtable};

typedef struct matrix_fixture {
  fake_executor_t fake;
  h2_runtime_t runtime;
  h2_h2loader_e2e_config_t config;
  h2_h2loader_host_catalog_entry_t assets[2][5];
  int bad_data;
} matrix_fixture_t;

static h2_pal_result_t matrix_read(void *user, uint64_t offset, uint8_t *out,
                                    size_t len, size_t *got) {
  const h2_h2loader_e2e_package_t *package = user;
  if (offset > package->size) return H2_PAL_ERR_INVALID_ARG;
  if (len > package->size - (size_t)offset) len = package->size - (size_t)offset;
  memcpy(out, package->data + offset, len); *got = len;
  return H2_PAL_OK;
}

static h2_pal_result_t matrix_execute(void *user,
    h2_h2loader_e2e_transport_t transport, h2_h2loader_e2e_case_t test_case,
    h2_h2loader_e2e_case_result_t *out) {
  matrix_fixture_t *fixture = user;
  h2_pal_result_t rc = execute_case(&fixture->fake, transport, test_case, out);
  if (test_case < H2_H2LOADER_E2E_CASE_TAR_ZLIB_BASELINE) return rc;
  size_t offset = (size_t)(test_case - H2_H2LOADER_E2E_CASE_TAR_ZLIB_BASELINE);
  size_t format = offset / 5u, index = offset % 5u;
  const h2_h2loader_host_catalog_entry_t *asset = &fixture->assets[format][index];
  out->status_valid = out->data_checksum_valid = 1u;
  out->checksum_expectations_valid = index != 0u;
  out->expected_update_app = index == 2u || index == 4u;
  out->expected_update_data = index == 3u || index == 4u;
  strcpy(out->data_sha256, asset->data_sha256);
  if (fixture->bad_data && format == 0u && index == 3u)
    strcpy(out->data_sha256, fixture->assets[0][2].data_sha256);
  if (index != 0u) {
    strcpy(out->before_image_sha256, fixture->assets[format][index - 1u].image_sha256);
    strcpy(out->before_data_sha256, fixture->assets[format][index - 1u].data_sha256);
  }
  strcpy(out->status.board, asset->board);
  strcpy(out->status.target, asset->target);
  out->status.running_partition = 2u;
  out->status.active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
  strcpy(out->status.active_version, asset->version);
  strcpy(out->status.active_checksum, asset->image_sha256);
  h2_h2loader_host_metadata_t *metadata = &out->status.partition_2;
  metadata->valid = 1u;
  metadata->role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
  strcpy(metadata->board, asset->board); strcpy(metadata->target, asset->target);
  strcpy(metadata->version, asset->version);
  strcpy(metadata->image_checksum, asset->image_sha256);
  strcpy(metadata->package_checksum, asset->sha256);
  return rc;
}

static void matrix_init(matrix_fixture_t *fixture) {
  static const char *names[] = {"baseline", "unchanged", "app-only", "data-only", "both-changed"};
  memset(fixture, 0, sizeof(*fixture));
  fixture->runtime.mem = &matrix_mem;
  fixture->config.runtime = &fixture->runtime;
  fixture->config.uart_endpoint = "/dev/test";
  fixture->config.ble_endpoint = "4:001122334455";
  fixture->config.repeat_count = 1u;
  fixture->config.checksum_formats = 3u;
  fixture->config.execute_case = matrix_execute;
  fixture->config.execute_user = fixture;
  for (size_t format = 0u; format < 2u; ++format)
    for (size_t index = 0u; index < 5u; ++index) {
      char path[2048];
      int n = snprintf(path, sizeof(path), "%s/%s/projects/h2loader/apps/e2e-runner/tests/checksum-fixtures/%s/%s%s",
          getenv("TEST_SRCDIR"), getenv("TEST_WORKSPACE"), format ? "zlib_tar" : "tar_zlib",
          names[index], format ? ".update.tar" : ".update.tar.zlib");
      assert(n > 0 && (size_t)n < sizeof(path));
      FILE *file = fopen(path, "rb"); assert(file != NULL);
      assert(fseek(file, 0, SEEK_END) == 0);
      long length = ftell(file); assert(length > 0);
      assert(fseek(file, 0, SEEK_SET) == 0);
      uint8_t *data = malloc((size_t)length); assert(data != NULL);
      assert(fread(data, 1u, (size_t)length, file) == (size_t)length);
      assert(fclose(file) == 0);
      fixture->config.checksum_packages[format][index] = (h2_h2loader_e2e_package_t){data, (size_t)length};
      h2_h2loader_host_package_inspect_config_t inspect = {
          .allocator = &matrix_mem, .payload_bytes = (uint64_t)length,
          .read_payload = matrix_read, .payload_user = &fixture->config.checksum_packages[format][index]};
      assert(h2_h2loader_host_package_inspect(&inspect, &fixture->assets[format][index]) == H2_PAL_OK);
    }
}

static void matrix_close(matrix_fixture_t *fixture) {
  for (size_t format = 0u; format < 2u; ++format)
    for (size_t index = 0u; index < 5u; ++index)
      free((void *)fixture->config.checksum_packages[format][index].data);
}

static void test_checksum_matrix_and_false_success(void) {
  static matrix_fixture_t fixture;
  static h2_h2loader_e2e_result_t result;
  matrix_init(&fixture);
  assert(h2_h2loader_e2e_run(&fixture.config, &result) == H2_PAL_OK);
  assert(result.case_count == 30u && result.passed == 30u);
  assert(strcmp(h2_h2loader_e2e_case_name(result.cases[6].test_case), "tar-zlib-unchanged") == 0);
  assert(result.cases[14].package_format == 2u && result.cases[14].data_checksum_valid);
  fixture.fake.count = 0u;
  fixture.bad_data = 1;
  assert(h2_h2loader_e2e_run(&fixture.config, &result) == H2_PAL_ERR_INVALID_STATE);
  assert(result.cases[8].result == H2_PAL_ERR_INVALID_STATE);
  assert(!result.cases[8].data_checksum_valid);
  assert(result.cases[9].result == H2_PAL_ERR_INVALID_STATE);
  /* A valid but unguarded new packet must not claim skip proof. */
  h2_h2loader_e2e_package_t saved = fixture.config.checksum_packages[1][1];
  fixture.config.checksum_packages[1][1] = fixture.config.checksum_packages[1][0];
  fixture.fake.count = 0u;
  assert(h2_h2loader_e2e_run(&fixture.config, &result) == H2_PAL_ERR_FORMAT);
  assert(result.case_count == 0u && fixture.fake.count == 0u);
  fixture.config.checksum_packages[1][1] = saved;
  matrix_close(&fixture);
}

int main(void) {
  test_checksum_matrix_and_false_success();
  test_full_sequence_for_both_transports();
  test_failure_is_reported_without_hiding_cleanup();
  test_memory_follows_authoritative_availability();
  test_legacy_check_uses_preceding_status_availability();
  test_invalid_configs();
  test_monitor_cases_are_uart_only_and_bounded();
  test_monitor_runs_each_reboot_with_a_bootable_target();
  test_crash_app_runs_once_before_cross_transport_coredump();
  test_names();
  test_cancel_does_not_execute_later_cases();
  return 0;
}
