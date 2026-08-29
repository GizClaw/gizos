#include "h2_h2loader_e2e_runner.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fake_executor {
  size_t count;
  h2_h2loader_e2e_case_t cases[32];
  h2_h2loader_e2e_transport_t transports[32];
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
  return H2_PAL_OK;
}

static void test_full_sequence_for_both_transports(void) {
  fake_executor_t fake = {0};
  h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .ble_endpoint = "4:001122334455",
      .firmware = (const uint8_t *)"x",
      .firmware_size = 1u,
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
      .include_rollback = 1u,
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
  assert(result.case_count == 18u);
  assert(result.passed == 18u);
  assert(result.failed == 0u);
  assert(fake.count == 18u);
  for (size_t i = 0u; i < 9u; ++i) {
    assert(fake.transports[i] == H2_H2LOADER_E2E_TRANSPORT_UART);
    assert(fake.transports[i + 9u] == H2_H2LOADER_E2E_TRANSPORT_BLE);
    assert(fake.cases[i] == fake.cases[i + 9u]);
  }
}

static h2_pal_result_t fail_send(void *user,
                                 h2_h2loader_e2e_transport_t transport,
                                 h2_h2loader_e2e_case_t test_case,
                                 h2_h2loader_e2e_case_result_t *out_result) {
  (void)user;
  (void)transport;
  (void)out_result;
  return test_case == H2_H2LOADER_E2E_CASE_SEND ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static void test_failure_is_reported_without_hiding_cleanup(void) {
  h2_h2loader_e2e_result_t result;
  const h2_h2loader_e2e_config_t config = {
      .uart_endpoint = "/dev/test",
      .firmware = (const uint8_t *)"x",
      .firmware_size = 1u,
      .repeat_count = 1u,
      .include_send = 1u,
      .execute_case = fail_send,
  };
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_IO);
  assert(result.case_count == 3u);
  assert(result.failed == 1u);
  assert(result.cases[2].test_case ==
         H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND);
  assert(result.cases[2].result == H2_PAL_OK);
}

static void test_invalid_configs(void) {
  h2_h2loader_e2e_result_t result;
  h2_h2loader_e2e_config_t config = {.repeat_count = 1u};
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_INVALID_ARG);
  config.uart_endpoint = "/dev/test";
  config.include_wifi = 1u;
  config.execute_case = execute_case;
  assert(h2_h2loader_e2e_run(&config, &result) == H2_PAL_ERR_INVALID_ARG);
}

static void test_names(void) {
  assert(strcmp(h2_h2loader_e2e_transport_name(H2_H2LOADER_E2E_TRANSPORT_UART),
                "uart") == 0);
  assert(strcmp(h2_h2loader_e2e_case_name(H2_H2LOADER_E2E_CASE_SEND_URL),
                "send-url") == 0);
}

int main(void) {
  test_full_sequence_for_both_transports();
  test_failure_is_reported_without_hiding_cleanup();
  test_invalid_configs();
  test_names();
  return 0;
}
