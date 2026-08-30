#ifndef H2_H2LOADER_E2E_RUNNER_H
#define H2_H2LOADER_E2E_RUNNER_H

#include "h2_h2loader_host.h"
#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_H2LOADER_E2E_MAX_CASES 512u

typedef enum h2_h2loader_e2e_transport {
  H2_H2LOADER_E2E_TRANSPORT_UART = 1,
  H2_H2LOADER_E2E_TRANSPORT_BLE = 2,
} h2_h2loader_e2e_transport_t;

typedef enum h2_h2loader_e2e_case {
  H2_H2LOADER_E2E_CASE_STATUS = 1,
  H2_H2LOADER_E2E_CASE_WIFI_SCAN,
  H2_H2LOADER_E2E_CASE_WIFI_CONNECT,
  H2_H2LOADER_E2E_CASE_WIFI_DISCONNECT,
  H2_H2LOADER_E2E_CASE_SEND,
  H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND,
  H2_H2LOADER_E2E_CASE_SEND_URL,
  H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND_URL,
  H2_H2LOADER_E2E_CASE_REBOOT_APP_PRESERVES_STAGE,
  H2_H2LOADER_E2E_CASE_REBOOT_LOADER_PRESERVES_STAGE,
  H2_H2LOADER_E2E_CASE_INSTALL_APP,
  H2_H2LOADER_E2E_CASE_INSTALL_LOADER,
} h2_h2loader_e2e_case_t;

typedef struct h2_h2loader_e2e_case_result {
  h2_h2loader_e2e_transport_t transport;
  h2_h2loader_e2e_case_t test_case;
  uint32_t iteration;
  h2_pal_result_t result;
  h2_h2loader_host_command_terminal_t terminal;
  uint64_t elapsed_ms;
  uint64_t acknowledged_bytes;
  uint64_t total_bytes;
  size_t output_bytes;
  uint8_t status_valid;
  h2_h2loader_host_status_t status;
} h2_h2loader_e2e_case_result_t;

typedef h2_pal_result_t (*h2_h2loader_e2e_case_executor_fn)(
    void *user, h2_h2loader_e2e_transport_t transport,
    h2_h2loader_e2e_case_t test_case,
    h2_h2loader_e2e_case_result_t *out_result);

typedef void (*h2_h2loader_e2e_case_event_fn)(
    void *user, const h2_h2loader_e2e_case_result_t *result, int started);

typedef void (*h2_h2loader_e2e_progress_event_fn)(
    void *user, const h2_h2loader_e2e_case_result_t *result);

typedef struct h2_h2loader_e2e_config {
  h2_runtime_t *runtime;
  const h2_pal_serial_host_api_t *serial;
  const h2_pal_ble_host_api_t *ble;
  const char *uart_endpoint;
  const char *ble_endpoint;
  const char *expected_board;
  const char *expected_target;

  const uint8_t *app_firmware;
  size_t app_firmware_size;
  const uint8_t *loader_firmware;
  size_t loader_firmware_size;
  const char *firmware_url;
  uint64_t firmware_url_bytes;
  const char *firmware_url_sha256;
  const char *wifi_ssid;
  const char *wifi_password;

  uint32_t repeat_count;
  uint32_t wait_timeout_ms;
  uint32_t command_timeout_ms;
  uint8_t include_wifi;
  uint8_t include_send;
  uint8_t include_send_url;
  uint8_t include_lifecycle;

  h2_h2loader_host_cancelled_fn is_cancelled;
  void *cancel_user;
  h2_h2loader_e2e_case_event_fn on_case;
  void *case_user;
  h2_h2loader_e2e_progress_event_fn on_progress;
  void *progress_user;

  /** Optional deterministic executor used by unit tests and custom targets. */
  h2_h2loader_e2e_case_executor_fn execute_case;
  void *execute_user;
} h2_h2loader_e2e_config_t;

typedef struct h2_h2loader_e2e_result {
  h2_pal_result_t result;
  h2_h2loader_e2e_case_result_t cases[H2_H2LOADER_E2E_MAX_CASES];
  size_t case_count;
  size_t passed;
  size_t failed;
  uint64_t elapsed_ms;
  uint64_t app_firmware_bytes;
  char app_firmware_sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
  uint64_t loader_firmware_bytes;
  char loader_firmware_sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
  int complete;
} h2_h2loader_e2e_result_t;

const char *
h2_h2loader_e2e_transport_name(h2_h2loader_e2e_transport_t transport);
const char *h2_h2loader_e2e_case_name(h2_h2loader_e2e_case_t test_case);

/** Run the selected cases once per transport for repeat_count iterations. */
h2_pal_result_t h2_h2loader_e2e_run(const h2_h2loader_e2e_config_t *config,
                                    h2_h2loader_e2e_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
