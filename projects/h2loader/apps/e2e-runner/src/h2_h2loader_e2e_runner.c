#include "h2_h2loader_e2e_runner.h"

#include "h2_h2loader_host_package.h"

#include <string.h>

#define H2_E2E_BLE_CANDIDATE_CAPACITY 32u
#define H2_E2E_DEFAULT_WAIT_TIMEOUT_MS 60000u
#define H2_E2E_DEFAULT_COMMAND_TIMEOUT_MS 120000u
#define H2_E2E_COMMAND_OUTPUT_CAPACITY 4096u
#define H2_E2E_WIFI_SCAN_ATTEMPTS 3u
#define H2_E2E_WIFI_SCAN_RETRY_DELAY_MS 500u

typedef struct h2_e2e_transport_context {
  const h2_h2loader_e2e_config_t *config;
  h2_h2loader_e2e_transport_t transport;
  h2_h2loader_host_serial_connection_t *serial_connection;
  h2_h2loader_host_ble_connection_t *ble_connection;
  h2_h2loader_host_candidate_t ble_candidate;
  h2_h2loader_host_catalog_entry_t firmware_asset;
  h2_h2loader_e2e_case_result_t *case_result;
  char command_output[H2_E2E_COMMAND_OUTPUT_CAPACITY];
  size_t command_output_size;
  uint32_t last_reported_percent;
} h2_e2e_transport_context_t;

typedef struct h2_e2e_memory_source {
  const uint8_t *data;
  size_t size;
} h2_e2e_memory_source_t;

const char *
h2_h2loader_e2e_transport_name(h2_h2loader_e2e_transport_t transport) {
  if (transport == H2_H2LOADER_E2E_TRANSPORT_UART)
    return "uart";
  if (transport == H2_H2LOADER_E2E_TRANSPORT_BLE)
    return "ble";
  return "unknown";
}

const char *h2_h2loader_e2e_case_name(h2_h2loader_e2e_case_t test_case) {
  switch (test_case) {
  case H2_H2LOADER_E2E_CASE_STATUS:
    return "status";
  case H2_H2LOADER_E2E_CASE_WIFI_SCAN:
    return "wifi-scan";
  case H2_H2LOADER_E2E_CASE_WIFI_CONNECT:
    return "wifi-connect";
  case H2_H2LOADER_E2E_CASE_WIFI_DISCONNECT:
    return "wifi-disconnect";
  case H2_H2LOADER_E2E_CASE_SEND:
    return "send";
  case H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND:
    return "stage-abort-after-send";
  case H2_H2LOADER_E2E_CASE_SEND_URL:
    return "send-url";
  case H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND_URL:
    return "stage-abort-after-send-url";
  case H2_H2LOADER_E2E_CASE_REBOOT_LOADER:
    return "reboot-loader";
  default:
    return "unknown";
  }
}

static uint32_t timeout_or(uint32_t value, uint32_t fallback) {
  return value == 0u ? fallback : value;
}

static int text_matches(const char *expected, const char *actual) {
  return expected == NULL || expected[0] == '\0' ||
         (actual != NULL && strcmp(expected, actual) == 0);
}

static int sha256_valid(const char *value) {
  if (value == NULL || strlen(value) != H2_H2LOADER_HOST_SHA256_HEX_LEN) {
    return 0;
  }
  for (size_t i = 0u; i < H2_H2LOADER_HOST_SHA256_HEX_LEN; ++i) {
    if (!((value[i] >= '0' && value[i] <= '9') ||
          (value[i] >= 'a' && value[i] <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

static int cancelled(const h2_h2loader_e2e_config_t *config) {
  return config->is_cancelled != NULL &&
         config->is_cancelled(config->cancel_user);
}

static h2_pal_result_t now_ms(const h2_h2loader_e2e_config_t *config,
                              uint64_t *out) {
  if (config->runtime == NULL || config->runtime->time == NULL) {
    *out = 0u;
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return h2_pal_time_get_monotonic_ms(config->runtime->time, out);
}

static h2_pal_result_t memory_read(void *user, uint64_t offset, uint8_t *out,
                                   size_t out_size, size_t *out_read) {
  const h2_e2e_memory_source_t *source = user;
  if (source == NULL || out_read == NULL || (out == NULL && out_size != 0u) ||
      offset > source->size) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  size_t available = source->size - (size_t)offset;
  size_t count = out_size < available ? out_size : available;
  if (count != 0u)
    memcpy(out, source->data + (size_t)offset, count);
  *out_read = count;
  return H2_PAL_OK;
}

static h2_pal_result_t count_output(void *user, const uint8_t *data,
                                    size_t len) {
  h2_e2e_transport_context_t *context = user;
  context->case_result->output_bytes += len;
  size_t available =
      sizeof(context->command_output) - 1u - context->command_output_size;
  size_t count = len < available ? len : available;
  if (count != 0u) {
    memcpy(context->command_output + context->command_output_size, data, count);
    context->command_output_size += count;
    context->command_output[context->command_output_size] = '\0';
  }
  return H2_PAL_OK;
}

static int scan_contains_ssid(const h2_e2e_transport_context_t *context) {
  static const char hex[] = "0123456789abcdef";
  const unsigned char *ssid = (const unsigned char *)context->config->wifi_ssid;
  char marker[2u * H2_H2LOADER_HOST_IDENTITY_MAX_LEN + 16u];
  size_t ssid_size = strlen(context->config->wifi_ssid);
  if (ssid_size > (sizeof(marker) - sizeof("ssid_hex=")) / 2u)
    return 0;
  memcpy(marker, "ssid_hex=", sizeof("ssid_hex=") - 1u);
  size_t offset = sizeof("ssid_hex=") - 1u;
  for (size_t i = 0u; i < ssid_size; ++i) {
    marker[offset++] = hex[ssid[i] >> 4u];
    marker[offset++] = hex[ssid[i] & 0x0fu];
  }
  marker[offset] = '\0';
  return strstr(context->command_output, marker) != NULL;
}

static void record_progress(void *user, uint64_t acknowledged, uint64_t total) {
  h2_e2e_transport_context_t *context = user;
  context->case_result->acknowledged_bytes = acknowledged;
  context->case_result->total_bytes = total;
  uint32_t percent =
      total == 0u ? 0u : (uint32_t)((100u * acknowledged) / total);
  if (context->config->on_progress != NULL &&
      percent > context->last_reported_percent &&
      (percent == 100u || percent >= context->last_reported_percent + 5u)) {
    context->last_reported_percent = percent;
    context->config->on_progress(context->config->progress_user,
                                 context->case_result);
  }
}

static h2_pal_result_t
disconnect_transport(h2_e2e_transport_context_t *context) {
  h2_pal_result_t serial_result =
      h2_h2loader_host_serial_disconnect(&context->serial_connection);
  h2_pal_result_t ble_result =
      h2_h2loader_host_ble_disconnect(&context->ble_connection);
  return serial_result == H2_PAL_OK ? ble_result : serial_result;
}

static h2_pal_result_t
resolve_ble_candidate(h2_e2e_transport_context_t *context) {
  h2_h2loader_host_candidate_t candidates[H2_E2E_BLE_CANDIDATE_CAPACITY];
  h2_h2loader_host_scan_result_t result;
  size_t matches = 0u;
  memset(candidates, 0, sizeof(candidates));
  memset(&result, 0, sizeof(result));
  h2_h2loader_host_scan_config_t scan = {
      .ble = context->config->ble,
      .sync = context->config->runtime->sync,
      .time = context->config->runtime->time,
      .ble_timeout_ms = timeout_or(context->config->wait_timeout_ms,
                                   H2_E2E_DEFAULT_WAIT_TIMEOUT_MS),
      .ble_endpoint = context->config->ble_endpoint,
      .candidates = candidates,
      .candidate_capacity = H2_E2E_BLE_CANDIDATE_CAPACITY,
  };
  h2_pal_result_t rc = h2_h2loader_host_scan(&scan, &result);
  if (rc != H2_PAL_OK)
    return rc;
  memset(&context->ble_candidate, 0, sizeof(context->ble_candidate));
  for (size_t i = 0u; i < result.count; ++i) {
    if (candidates[i].transport == H2_H2LOADER_HOST_TRANSPORT_BLE &&
        strcmp(candidates[i].endpoint, context->config->ble_endpoint) == 0) {
      context->ble_candidate = candidates[i];
      ++matches;
    }
  }
  if (matches == 0u)
    return H2_PAL_ERR_NOT_FOUND;
  return matches == 1u ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t
connect_transport(h2_e2e_transport_context_t *context,
                  h2_h2loader_host_status_t *out_status) {
  const h2_h2loader_e2e_config_t *config = context->config;
  h2_pal_result_t rc = disconnect_transport(context);
  if (rc != H2_PAL_OK)
    return rc;
  memset(out_status, 0, sizeof(*out_status));
  if (context->transport == H2_H2LOADER_E2E_TRANSPORT_UART) {
    const h2_h2loader_host_serial_connection_config_t connect = {
        .serial = config->serial,
        .time = config->runtime->time,
        .allocator = config->runtime->mem,
        .port_id = config->uart_endpoint,
        .handshake_timeout_ms =
            timeout_or(config->wait_timeout_ms, H2_E2E_DEFAULT_WAIT_TIMEOUT_MS),
        .command_timeout_ms = timeout_or(config->command_timeout_ms,
                                         H2_E2E_DEFAULT_COMMAND_TIMEOUT_MS),
    };
    rc = h2_h2loader_host_serial_connect(&connect, &context->serial_connection);
    if (rc == H2_PAL_OK) {
      rc = h2_h2loader_host_serial_read_status(context->serial_connection,
                                               out_status);
    }
  } else {
    rc = resolve_ble_candidate(context);
    if (rc == H2_PAL_OK) {
      const char *advertised_board =
          strncmp(context->ble_candidate.advertised_board, "fnv1a64:", 8u) == 0
              ? NULL
              : context->ble_candidate.advertised_board;
      const h2_h2loader_host_ble_connection_config_t connect = {
          .ble = config->ble,
          .task = config->runtime->task,
          .time = config->runtime->time,
          .sync = config->runtime->sync,
          .system_event = config->runtime->system_event,
          .allocator = config->runtime->mem,
          .address = context->ble_candidate.ble_address,
          .advertised_board = advertised_board,
          .connect_timeout_ms = timeout_or(config->wait_timeout_ms,
                                           H2_E2E_DEFAULT_WAIT_TIMEOUT_MS),
          .command_timeout_ms = timeout_or(config->command_timeout_ms,
                                           H2_E2E_DEFAULT_COMMAND_TIMEOUT_MS),
      };
      rc = h2_h2loader_host_ble_connect(&connect, &context->ble_connection,
                                        out_status);
    }
  }
  if (rc == H2_PAL_OK &&
      (!text_matches(config->expected_board, out_status->board) ||
       !text_matches(config->expected_target, out_status->target))) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK && context->case_result != NULL) {
    context->case_result->status = *out_status;
    context->case_result->status_valid = 1u;
  }
  if (rc != H2_PAL_OK)
    (void)disconnect_transport(context);
  return rc;
}

static h2_pal_result_t read_status(h2_e2e_transport_context_t *context,
                                   h2_h2loader_host_status_t *out_status) {
  return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
             ? h2_h2loader_host_serial_read_status(context->serial_connection,
                                                   out_status)
             : h2_h2loader_host_ble_read_status(context->ble_connection,
                                                out_status);
}

static h2_pal_result_t
execute_command(h2_e2e_transport_context_t *context,
                const h2_h2loader_host_command_request_t *request,
                h2_h2loader_host_command_result_t *out_result) {
  return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
             ? h2_h2loader_host_serial_execute_command(
                   context->serial_connection, request, out_result)
             : h2_h2loader_host_ble_execute_command(context->ble_connection,
                                                    request, out_result);
}

static h2_pal_result_t
stage_payload(h2_e2e_transport_context_t *context,
              const h2_h2loader_host_catalog_entry_t *asset,
              h2_h2loader_host_payload_read_fn read_payload, void *payload_user,
              h2_h2loader_host_cancelled_fn is_cancelled, void *cancel_user,
              h2_h2loader_host_progress_fn on_progress, void *progress_user) {
  return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
             ? h2_h2loader_host_serial_stage(context->serial_connection, asset,
                                             read_payload, payload_user,
                                             is_cancelled, cancel_user,
                                             on_progress, progress_user)
             : h2_h2loader_host_ble_stage(
                   context->ble_connection, asset, read_payload, payload_user,
                   is_cancelled, cancel_user, on_progress, progress_user);
}

static h2_pal_result_t managed_connect(void *user,
                                       h2_h2loader_host_status_t *out_status) {
  return connect_transport(user, out_status);
}

static h2_pal_result_t
managed_stage(void *user, const h2_h2loader_host_catalog_entry_t *asset,
              h2_h2loader_host_payload_read_fn read_payload, void *payload_user,
              h2_h2loader_host_cancelled_fn is_cancelled, void *cancel_user,
              h2_h2loader_host_progress_fn on_progress, void *progress_user) {
  return stage_payload(user, asset, read_payload, payload_user, is_cancelled,
                       cancel_user, on_progress, progress_user);
}

static h2_pal_result_t
managed_read_status(void *user, h2_h2loader_host_status_t *out_status) {
  return read_status(user, out_status);
}

static h2_pal_result_t managed_disconnect(void *user) {
  return disconnect_transport(user);
}

static h2_pal_result_t managed_rediscover(void *user) {
  const h2_e2e_transport_context_t *context = user;
  return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
             ? H2_PAL_OK
             : H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t run_status(h2_e2e_transport_context_t *context) {
  h2_h2loader_host_status_t status;
  h2_pal_result_t rc = connect_transport(context, &status);
  h2_pal_result_t close_rc = disconnect_transport(context);
  return rc == H2_PAL_OK ? close_rc : rc;
}

static h2_pal_result_t run_simple_command(h2_e2e_transport_context_t *context,
                                          h2_h2loader_host_command_t command) {
  h2_h2loader_host_status_t status;
  h2_h2loader_host_command_result_t result = {0};
  h2_pal_result_t rc = connect_transport(context, &status);
  if (rc == H2_PAL_OK) {
    const h2_h2loader_host_command_request_t request = {
        .command = command,
        .status = &status,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = count_output,
        .output_user = context,
    };
    rc = execute_command(context, &request, &result);
    context->case_result->terminal = result.terminal;
    if (rc == H2_PAL_OK &&
        result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
      rc = H2_PAL_ERR_IO;
    }
  }
  h2_pal_result_t close_rc = disconnect_transport(context);
  return rc == H2_PAL_OK ? close_rc : rc;
}

static h2_pal_result_t run_wifi_command(h2_e2e_transport_context_t *context,
                                        h2_h2loader_host_command_t command) {
  uint32_t attempts = command == H2_H2LOADER_HOST_COMMAND_WIFI_SCAN
                          ? H2_E2E_WIFI_SCAN_ATTEMPTS
                          : 1u;
  h2_pal_result_t rc = H2_PAL_ERR_NOT_FOUND;
  for (uint32_t attempt = 0u; attempt < attempts; ++attempt) {
    h2_h2loader_host_status_t status;
    h2_h2loader_host_command_result_t result = {0};
    context->command_output_size = 0u;
    context->command_output[0] = '\0';
    rc = connect_transport(context, &status);
    if (rc != H2_PAL_OK)
      break;
    const h2_h2loader_host_command_request_t request = {
        .command = command,
        .status = &status,
        .ssid = command == H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT
                    ? context->config->wifi_ssid
                    : NULL,
        .password = command == H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT
                        ? context->config->wifi_password
                        : NULL,
        .wifi_scan_limit = H2_H2LOADER_HOST_WIFI_SCAN_DEFAULT_LIMIT,
        .wifi_scan_timeout_ms = H2_H2LOADER_HOST_WIFI_SCAN_DEFAULT_TIMEOUT_MS,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = count_output,
        .output_user = context,
    };
    rc = execute_command(context, &request, &result);
    context->case_result->terminal = result.terminal;
    if (rc == H2_PAL_OK &&
        result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
      rc = H2_PAL_ERR_IO;
    }
    h2_pal_result_t close_rc = disconnect_transport(context);
    if (rc == H2_PAL_OK)
      rc = close_rc;
    if (rc != H2_PAL_OK || command != H2_H2LOADER_HOST_COMMAND_WIFI_SCAN ||
        scan_contains_ssid(context)) {
      break;
    }
    rc = H2_PAL_ERR_NOT_FOUND;
    if (attempt + 1u < attempts) {
      h2_pal_result_t sleep_rc = h2_pal_time_sleep_ms(
          context->config->runtime->time, H2_E2E_WIFI_SCAN_RETRY_DELAY_MS);
      if (sleep_rc != H2_PAL_OK) {
        rc = sleep_rc;
        break;
      }
    }
  }
  return rc;
}

static h2_pal_result_t run_send(h2_e2e_transport_context_t *context) {
  const h2_e2e_memory_source_t source = {
      .data = context->config->firmware,
      .size = context->config->firmware_size,
  };
  static const h2_h2loader_host_managed_transport_vtable_t vtable = {
      .connect = managed_connect,
      .stage = managed_stage,
      .read_status = managed_read_status,
      .disconnect = managed_disconnect,
      .rediscover = managed_rediscover,
  };
  h2_h2loader_host_status_t final_status;
  const h2_h2loader_host_managed_operation_config_t operation = {
      .time = context->config->runtime->time,
      .transport = {.user = context, .vtable = &vtable},
      .asset = &context->firmware_asset,
      .read_payload = memory_read,
      .payload_user = (void *)&source,
      .is_cancelled = context->config->is_cancelled,
      .cancel_user = context->config->cancel_user,
      .on_progress = record_progress,
      .progress_user = context,
      .reconnect_delay_ms = 250u,
      .reconnect_attempts = 240u,
  };
  context->case_result->total_bytes = context->firmware_asset.bytes;
  h2_pal_result_t rc =
      h2_h2loader_host_stage_operation_run(&operation, &final_status);
  if (rc == H2_PAL_OK) {
    context->case_result->status = final_status;
    context->case_result->status_valid = 1u;
  }
  return rc;
}

static h2_pal_result_t run_send_url(h2_e2e_transport_context_t *context) {
  h2_h2loader_host_status_t initial_status;
  h2_h2loader_host_status_t final_status;
  h2_h2loader_host_command_result_t result = {0};
  h2_pal_result_t rc = connect_transport(context, &initial_status);
  if (rc == H2_PAL_OK && context->config->wifi_ssid != NULL) {
    const h2_h2loader_host_command_request_t wifi = {
        .command = H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT,
        .status = &initial_status,
        .ssid = context->config->wifi_ssid,
        .password = context->config->wifi_password,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = count_output,
        .output_user = context,
    };
    rc = execute_command(context, &wifi, &result);
    if (rc == H2_PAL_OK &&
        result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
      rc = H2_PAL_ERR_IO;
    }
  }
  if (rc == H2_PAL_OK) {
    memset(&result, 0, sizeof(result));
    const h2_h2loader_host_command_request_t stage = {
        .command = H2_H2LOADER_HOST_COMMAND_STAGE_URL,
        .status = &initial_status,
        .url = context->config->firmware_url,
        .expected_bytes = context->config->firmware_url_bytes,
        .expected_sha256 = context->config->firmware_url_sha256,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = count_output,
        .output_user = context,
    };
    rc = execute_command(context, &stage, &result);
    context->case_result->terminal = result.terminal;
    if (rc == H2_PAL_OK &&
        result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
      rc = H2_PAL_ERR_IO;
    }
  }
  if (rc == H2_PAL_OK) {
    memset(&final_status, 0, sizeof(final_status));
    rc = read_status(context, &final_status);
  }
  if (rc == H2_PAL_OK &&
      (strcmp(final_status.board, initial_status.board) != 0 ||
       strcmp(final_status.target, initial_status.target) != 0 ||
       !final_status.stage.valid ||
       final_status.stage.package_size != context->config->firmware_url_bytes ||
       strcmp(final_status.stage.package_checksum,
              context->config->firmware_url_sha256) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    context->case_result->status = final_status;
    context->case_result->status_valid = 1u;
  }
  context->case_result->total_bytes = context->config->firmware_url_bytes;
  context->case_result->acknowledged_bytes =
      rc == H2_PAL_OK ? context->config->firmware_url_bytes : 0u;
  h2_pal_result_t close_rc = disconnect_transport(context);
  return rc == H2_PAL_OK ? close_rc : rc;
}

static h2_pal_result_t execute_real_case(h2_e2e_transport_context_t *context,
                                         h2_h2loader_e2e_case_t test_case) {
  switch (test_case) {
  case H2_H2LOADER_E2E_CASE_STATUS:
    return run_status(context);
  case H2_H2LOADER_E2E_CASE_WIFI_SCAN:
    return run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_SCAN);
  case H2_H2LOADER_E2E_CASE_WIFI_CONNECT:
    return run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT);
  case H2_H2LOADER_E2E_CASE_WIFI_DISCONNECT:
    return run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT);
  case H2_H2LOADER_E2E_CASE_SEND:
    return run_send(context);
  case H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND:
  case H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND_URL:
    return run_simple_command(context, H2_H2LOADER_HOST_COMMAND_STAGE_ABORT);
  case H2_H2LOADER_E2E_CASE_SEND_URL:
    return run_send_url(context);
  case H2_H2LOADER_E2E_CASE_REBOOT_LOADER:
    return run_simple_command(context, H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER);
  default:
    return H2_PAL_ERR_INVALID_ARG;
  }
}

static h2_pal_result_t
inspect_firmware(const h2_h2loader_e2e_config_t *config,
                 h2_h2loader_host_catalog_entry_t *out_asset) {
  const h2_e2e_memory_source_t source = {
      .data = config->firmware,
      .size = config->firmware_size,
  };
  const h2_h2loader_host_package_inspect_config_t inspect = {
      .allocator = config->runtime->mem,
      .read_payload = memory_read,
      .payload_user = (void *)&source,
      .payload_bytes = config->firmware_size,
  };
  return h2_h2loader_host_package_inspect(&inspect, out_asset);
}

static size_t cases_per_transport(const h2_h2loader_e2e_config_t *config) {
  size_t count = 1u;
  if (config->include_wifi)
    count += 3u;
  if (config->include_send)
    count += 2u;
  if (config->include_send_url)
    count += 2u;
  if (config->include_reboot_loader)
    count += 1u;
  return count;
}

static int config_valid(const h2_h2loader_e2e_config_t *config) {
  if (config == NULL || config->repeat_count == 0u ||
      (config->uart_endpoint == NULL && config->ble_endpoint == NULL)) {
    return 0;
  }
  if (config->execute_case == NULL &&
      (config->runtime == NULL || config->runtime->mem == NULL ||
       config->runtime->time == NULL ||
       (config->uart_endpoint != NULL && config->serial == NULL) ||
       (config->ble_endpoint != NULL &&
        (config->ble == NULL || config->runtime->task == NULL ||
         config->runtime->sync == NULL ||
         config->runtime->system_event == NULL)))) {
    return 0;
  }
  if (config->include_wifi &&
      (config->wifi_ssid == NULL || config->wifi_ssid[0] == '\0' ||
       config->wifi_password == NULL)) {
    return 0;
  }
  if (config->include_send &&
      (config->firmware == NULL || config->firmware_size == 0u)) {
    return 0;
  }
  if (config->include_send_url &&
      (config->firmware_url == NULL || config->firmware_url[0] == '\0' ||
       config->firmware_url_bytes == 0u ||
       !sha256_valid(config->firmware_url_sha256))) {
    return 0;
  }
  size_t transports = (config->uart_endpoint != NULL ? 1u : 0u) +
                      (config->ble_endpoint != NULL ? 1u : 0u);
  return cases_per_transport(config) * transports * config->repeat_count <=
         H2_H2LOADER_E2E_MAX_CASES;
}

static void append_case(const h2_h2loader_e2e_config_t *config,
                        h2_h2loader_e2e_result_t *result,
                        h2_e2e_transport_context_t *context,
                        h2_h2loader_e2e_transport_t transport,
                        h2_h2loader_e2e_case_t test_case, uint32_t iteration) {
  h2_h2loader_e2e_case_result_t *entry = &result->cases[result->case_count++];
  memset(entry, 0, sizeof(*entry));
  entry->transport = transport;
  entry->test_case = test_case;
  entry->iteration = iteration;
  context->case_result = entry;
  context->command_output_size = 0u;
  context->command_output[0] = '\0';
  context->last_reported_percent = 0u;
  if (config->on_case != NULL)
    config->on_case(config->case_user, entry, 1);
  uint64_t started = 0u;
  uint64_t finished = 0u;
  (void)now_ms(config, &started);
  entry->result = config->execute_case != NULL
                      ? config->execute_case(config->execute_user, transport,
                                             test_case, entry)
                      : execute_real_case(context, test_case);
  (void)now_ms(config, &finished);
  entry->elapsed_ms = finished >= started ? finished - started : 0u;
  if (entry->result == H2_PAL_OK) {
    ++result->passed;
  } else {
    ++result->failed;
    if (result->result == H2_PAL_OK)
      result->result = entry->result;
  }
  if (config->on_case != NULL)
    config->on_case(config->case_user, entry, 0);
}

static void run_transport_iteration(const h2_h2loader_e2e_config_t *config,
                                    h2_h2loader_e2e_result_t *result,
                                    h2_e2e_transport_context_t *context,
                                    h2_h2loader_e2e_transport_t transport,
                                    uint32_t iteration) {
  context->transport = transport;
  append_case(config, result, context, transport, H2_H2LOADER_E2E_CASE_STATUS,
              iteration);
  if (config->include_wifi) {
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_WIFI_SCAN, iteration);
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_WIFI_CONNECT, iteration);
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_WIFI_DISCONNECT, iteration);
  }
  if (config->include_send) {
    append_case(config, result, context, transport, H2_H2LOADER_E2E_CASE_SEND,
                iteration);
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND, iteration);
  }
  if (config->include_send_url) {
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_SEND_URL, iteration);
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND_URL, iteration);
  }
  if (config->include_reboot_loader) {
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_REBOOT_LOADER, iteration);
  }
}

h2_pal_result_t h2_h2loader_e2e_run(const h2_h2loader_e2e_config_t *config,
                                    h2_h2loader_e2e_result_t *out_result) {
  if (out_result != NULL)
    memset(out_result, 0, sizeof(*out_result));
  if (out_result == NULL || !config_valid(config)) {
    if (out_result != NULL) {
      out_result->result = H2_PAL_ERR_INVALID_ARG;
      out_result->complete = 1;
    }
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_result->result = H2_PAL_OK;
  h2_e2e_transport_context_t context = {.config = config};
  if (config->include_send && config->execute_case == NULL) {
    h2_pal_result_t rc = inspect_firmware(config, &context.firmware_asset);
    if (rc != H2_PAL_OK) {
      out_result->result = rc;
      out_result->complete = 1;
      return rc;
    }
    out_result->firmware_bytes = context.firmware_asset.bytes;
    memcpy(out_result->firmware_sha256, context.firmware_asset.sha256,
           sizeof(out_result->firmware_sha256));
  }
  uint64_t started = 0u;
  uint64_t finished = 0u;
  (void)now_ms(config, &started);
  for (uint32_t iteration = 1u;
       iteration <= config->repeat_count && !cancelled(config); ++iteration) {
    if (config->uart_endpoint != NULL) {
      run_transport_iteration(config, out_result, &context,
                              H2_H2LOADER_E2E_TRANSPORT_UART, iteration);
    }
    if (config->ble_endpoint != NULL && !cancelled(config)) {
      run_transport_iteration(config, out_result, &context,
                              H2_H2LOADER_E2E_TRANSPORT_BLE, iteration);
    }
  }
  (void)disconnect_transport(&context);
  if (cancelled(config) && out_result->result == H2_PAL_OK) {
    out_result->result = H2_PAL_EXIT;
  }
  (void)now_ms(config, &finished);
  out_result->elapsed_ms = finished >= started ? finished - started : 0u;
  out_result->complete = 1;
  return out_result->result;
}
