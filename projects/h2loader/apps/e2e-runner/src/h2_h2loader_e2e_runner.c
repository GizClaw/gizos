#include "h2_h2loader_e2e_runner.h"

#include "h2_h2loader_host_package.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define H2_E2E_BLE_CANDIDATE_CAPACITY 32u
#define H2_E2E_DEFAULT_WAIT_TIMEOUT_MS 60000u
#define H2_E2E_DEFAULT_COMMAND_TIMEOUT_MS 120000u
#define H2_E2E_COMMAND_OUTPUT_CAPACITY 4096u
#define H2_E2E_WIFI_SCAN_ATTEMPTS 3u
#define H2_E2E_WIFI_SCAN_RETRY_DELAY_MS 500u
#define H2_E2E_REMOVED_COMMAND_AVAILABILITY_MASK                            \
  ((UINT32_C(1) << 6) | (UINT32_C(1) << 7) | (UINT32_C(1) << 14) |         \
   (UINT32_C(1) << 15))

typedef struct h2_e2e_transport_context {
  const h2_h2loader_e2e_config_t *config;
  h2_h2loader_e2e_transport_t transport;
  h2_h2loader_host_serial_connection_t *serial_connection;
  h2_h2loader_host_ble_connection_t *ble_connection;
  h2_h2loader_host_candidate_t ble_candidate;
  char authoritative_device_uid[H2_H2LOADER_HOST_DEVICE_UID_MAX_LEN];
  uint8_t authoritative_device_uid_valid;
  h2_h2loader_host_catalog_entry_t app_asset;
  h2_h2loader_host_catalog_entry_t loader_asset;
  h2_h2loader_host_catalog_entry_t crash_asset;
  h2_h2loader_e2e_case_result_t *case_result;
  char command_output[H2_E2E_COMMAND_OUTPUT_CAPACITY];
  size_t command_output_size;
  uint32_t last_reported_percent;
  char coredump_line[512];
  size_t coredump_line_size;
  uint64_t coredump_decoded_bytes;
  uint64_t coredump_terminal_bytes;
  uint8_t coredump_terminal_seen;
  uint64_t expected_coredump_bytes;
  uint64_t monitor_deadline_ms;
  size_t monitor_output_bytes;
  uint8_t monitor_logs;
  uint32_t authoritative_command_availability;
  uint8_t authoritative_command_availability_valid;
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
  case H2_H2LOADER_E2E_CASE_HELP:
    return "help";
  case H2_H2LOADER_E2E_CASE_STATUS:
    return "status";
  case H2_H2LOADER_E2E_CASE_STATS:
    return "stats";
  case H2_H2LOADER_E2E_CASE_MEMORY:
    return "memory";
  case H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT:
    return "legacy-commands-absent";
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
  case H2_H2LOADER_E2E_CASE_REBOOT_APP_PRESERVES_STAGE:
    return "reboot-app-preserves-stage";
  case H2_H2LOADER_E2E_CASE_REBOOT_LOADER_PRESERVES_STAGE:
    return "reboot-loader-preserves-stage";
  case H2_H2LOADER_E2E_CASE_INSTALL_APP:
    return "install-app";
  case H2_H2LOADER_E2E_CASE_APP_HELP:
    return "app-help";
  case H2_H2LOADER_E2E_CASE_APP_STATUS:
    return "app-status";
  case H2_H2LOADER_E2E_CASE_APP_STATS:
    return "app-stats";
  case H2_H2LOADER_E2E_CASE_APP_MEMORY:
    return "app-memory";
  case H2_H2LOADER_E2E_CASE_APP_LEGACY_COMMANDS_ABSENT:
    return "app-legacy-commands-absent";
  case H2_H2LOADER_E2E_CASE_APP_WIFI_SCAN:
    return "app-wifi-scan";
  case H2_H2LOADER_E2E_CASE_APP_WIFI_CONNECT:
    return "app-wifi-connect";
  case H2_H2LOADER_E2E_CASE_APP_WIFI_DISCONNECT:
    return "app-wifi-disconnect";
  case H2_H2LOADER_E2E_CASE_APP_SEND:
    return "app-send";
  case H2_H2LOADER_E2E_CASE_APP_STAGE_ABORT_AFTER_SEND:
    return "app-stage-abort-after-send";
  case H2_H2LOADER_E2E_CASE_APP_SEND_URL:
    return "app-send-url";
  case H2_H2LOADER_E2E_CASE_APP_STAGE_ABORT_AFTER_SEND_URL:
    return "app-stage-abort-after-send-url";
  case H2_H2LOADER_E2E_CASE_INSTALL_LOADER:
    return "install-loader";
  case H2_H2LOADER_E2E_CASE_COREDUMP_STATUS:
    return "coredump-status";
  case H2_H2LOADER_E2E_CASE_COREDUMP_DUMP:
    return "coredump-dump";
  case H2_H2LOADER_E2E_CASE_COREDUMP_ERASE:
    return "coredump-erase";
  case H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE:
    return "coredump-status-after-erase";
  case H2_H2LOADER_E2E_CASE_MONITOR:
    return "monitor";
  case H2_H2LOADER_E2E_CASE_REBOOT_LOADER_MONITOR:
    return "reboot-loader-monitor";
  case H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR:
    return "reboot-app-monitor";
  case H2_H2LOADER_E2E_CASE_REBOOT_UPGRADE_MONITOR:
    return "reboot-upgrade-monitor";
  case H2_H2LOADER_E2E_CASE_INSTALL_CRASH_APP:
    return "install-crash-app";
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

static h2_pal_result_t monitor_output(void *user, const uint8_t *data,
                                      size_t len) {
  h2_e2e_transport_context_t *context = user;
  if (context == NULL || context->case_result == NULL ||
      (data == NULL && len != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  context->case_result->output_bytes += len;
  context->monitor_output_bytes += len;
  context->case_result->log_bytes += len;
  return context->config->on_log == NULL
             ? H2_PAL_OK
             : context->config->on_log(context->config->log_user, data, len);
}

static int monitor_cancelled(void *user) {
  h2_e2e_transport_context_t *context = user;
  uint64_t now = 0u;
  if (context == NULL || cancelled(context->config))
    return 1;
  return now_ms(context->config, &now) == H2_PAL_OK &&
         now >= context->monitor_deadline_ms;
}

static int parse_u64_decimal(const char *text, const char **out_end,
                             uint64_t *out_value) {
  uint64_t value = 0u;
  const char *cursor = text;
  if (text == NULL || out_end == NULL || out_value == NULL || *cursor < '0' ||
      *cursor > '9') {
    return 0;
  }
  while (*cursor >= '0' && *cursor <= '9') {
    const uint8_t digit = (uint8_t)(*cursor - '0');
    if (value > (UINT64_MAX - digit) / 10u)
      return 0;
    value = value * 10u + digit;
    ++cursor;
  }
  *out_end = cursor;
  *out_value = value;
  return 1;
}

static int hex_nibble(uint8_t value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

static h2_pal_result_t coredump_output_line(h2_e2e_transport_context_t *context,
                                            const char *line, size_t len) {
  static const char data_prefix[] = "H2_LOADER_COREDUMP_DATA offset=";
  static const char terminal_prefix[] =
      "H2_LOADER_COREDUMP_DUMP result=OK bytes=";
  if (len >= sizeof(data_prefix) - 1u &&
      memcmp(line, data_prefix, sizeof(data_prefix) - 1u) == 0) {
    const char *cursor = line + sizeof(data_prefix) - 1u;
    const char *end = NULL;
    uint64_t offset = 0u;
    if (!parse_u64_decimal(cursor, &end, &offset) ||
        offset != context->coredump_decoded_bytes ||
        (size_t)(line + len - end) < 5u || memcmp(end, " hex=", 5u) != 0) {
      return H2_PAL_ERR_FORMAT;
    }
    cursor = end + 5u;
    const size_t hex_size = (size_t)(line + len - cursor);
    if ((hex_size & 1u) != 0u || hex_size / 2u > 128u)
      return H2_PAL_ERR_FORMAT;
    for (size_t i = 0u; i < hex_size; ++i) {
      if (hex_nibble((uint8_t)cursor[i]) < 0)
        return H2_PAL_ERR_FORMAT;
    }
    context->coredump_decoded_bytes += hex_size / 2u;
    return H2_PAL_OK;
  }
  if (len >= sizeof(terminal_prefix) - 1u &&
      memcmp(line, terminal_prefix, sizeof(terminal_prefix) - 1u) == 0) {
    const char *end = NULL;
    if (!parse_u64_decimal(line + sizeof(terminal_prefix) - 1u, &end,
                           &context->coredump_terminal_bytes) ||
        context->coredump_terminal_bytes != context->coredump_decoded_bytes) {
      return H2_PAL_ERR_FORMAT;
    }
    context->coredump_terminal_seen = 1u;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t coredump_output(void *user, const uint8_t *data,
                                       size_t len) {
  h2_e2e_transport_context_t *context = user;
  if (context == NULL || (data == NULL && len != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  context->case_result->output_bytes += len;
  for (size_t i = 0u; i < len; ++i) {
    if (data[i] == '\n') {
      size_t line_len = context->coredump_line_size;
      if (line_len > 0u && context->coredump_line[line_len - 1u] == '\r')
        --line_len;
      h2_pal_result_t rc =
          coredump_output_line(context, context->coredump_line, line_len);
      context->coredump_line_size = 0u;
      if (rc != H2_PAL_OK)
        return rc;
      continue;
    }
    if (context->coredump_line_size == sizeof(context->coredump_line))
      return H2_PAL_ERR_NO_SPACE;
    context->coredump_line[context->coredump_line_size++] = (char)data[i];
  }
  return H2_PAL_OK;
}

static int parse_coredump_status(const char *output, uint64_t *out_bytes,
                                 int *out_blank) {
  static const char stored_marker[] = " stored_bytes=";
  static const char blank_marker[] = " blank=";
  const char *stored = strstr(output, stored_marker);
  const char *blank = strstr(output, blank_marker);
  const char *end = NULL;
  uint64_t blank_value = 0u;
  return stored != NULL && blank != NULL &&
         parse_u64_decimal(stored + sizeof(stored_marker) - 1u, &end,
                           out_bytes) &&
         parse_u64_decimal(blank + sizeof(blank_marker) - 1u, &end,
                           &blank_value) &&
         blank_value <= 1u && ((*out_blank = (int)blank_value), 1);
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
  if (matches != 1u)
    return H2_PAL_ERR_INVALID_STATE;
  return H2_PAL_OK;
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
        .baud_rate = config->uart_baud_rate,
        .handshake_timeout_ms =
            timeout_or(config->wait_timeout_ms, H2_E2E_DEFAULT_WAIT_TIMEOUT_MS),
        .command_timeout_ms = timeout_or(config->command_timeout_ms,
                                         H2_E2E_DEFAULT_COMMAND_TIMEOUT_MS),
        .on_log = monitor_output,
        .log_user = context,
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
  if (rc == H2_PAL_OK && context->transport == H2_H2LOADER_E2E_TRANSPORT_BLE) {
    if (!context->authoritative_device_uid_valid) {
      if (out_status->device_uid[0] != '\0' &&
          strcmp(out_status->device_uid, "unknown") != 0) {
        (void)snprintf(context->authoritative_device_uid,
                       sizeof(context->authoritative_device_uid), "%s",
                       out_status->device_uid);
        context->authoritative_device_uid_valid = 1u;
      }
    } else if (strcmp(context->authoritative_device_uid,
                      out_status->device_uid) != 0) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
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
  h2_e2e_transport_context_t *context = user;
  if (context->case_result != NULL) {
    ++context->case_result->reconnect_attempts;
  }
  if (context->transport == H2_H2LOADER_E2E_TRANSPORT_BLE &&
      !context->authoritative_device_uid_valid) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return context->transport == H2_H2LOADER_E2E_TRANSPORT_BLE
             ? resolve_ble_candidate(context)
             : H2_PAL_OK;
}

static h2_pal_result_t
managed_activate(void *user, const h2_h2loader_host_catalog_entry_t *asset) {
  h2_e2e_transport_context_t *context = user;
  return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
             ? h2_h2loader_host_serial_activate(context->serial_connection,
                                                asset)
             : h2_h2loader_host_ble_activate(context->ble_connection, asset);
}

static h2_pal_result_t run_status(h2_e2e_transport_context_t *context) {
  h2_h2loader_host_status_t status;
  h2_pal_result_t rc = connect_transport(context, &status);
  h2_pal_result_t close_rc = disconnect_transport(context);
  return rc == H2_PAL_OK ? close_rc : rc;
}

static h2_pal_result_t
begin_monitor_window(h2_e2e_transport_context_t *context) {
  uint64_t now = 0u;
  h2_pal_result_t rc = now_ms(context->config, &now);
  if (rc != H2_PAL_OK)
    return rc;
  context->monitor_deadline_ms = now + context->config->monitor_duration_ms;
  return context->monitor_deadline_ms >= now ? H2_PAL_OK
                                             : H2_PAL_ERR_INVALID_ARG;
}

static h2_pal_result_t
finish_bounded_monitor(h2_e2e_transport_context_t *context,
                       int require_output) {
  h2_pal_result_t rc = h2_h2loader_host_serial_monitor_logs(
      context->serial_connection, monitor_cancelled, context);
  if (rc == H2_PAL_EXIT && !cancelled(context->config))
    rc = H2_PAL_OK;
  if (rc == H2_PAL_OK && require_output && context->monitor_output_bytes == 0u)
    rc = H2_PAL_ERR_NOT_FOUND;
  return rc;
}

static h2_pal_result_t run_monitor(h2_e2e_transport_context_t *context) {
  h2_h2loader_host_status_t status;
  context->monitor_logs = 1u;
  h2_pal_result_t rc = connect_transport(context, &status);
  if (rc == H2_PAL_OK) {
    context->case_result->status = status;
    context->case_result->status_valid = 1u;
  }
  if (rc == H2_PAL_OK)
    rc = begin_monitor_window(context);
  if (rc == H2_PAL_OK)
    rc = finish_bounded_monitor(context, 0);
  h2_pal_result_t close_rc = disconnect_transport(context);
  context->monitor_logs = 0u;
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
  if (rc == H2_PAL_OK)
    rc = read_status(context, &status);
  if (rc == H2_PAL_OK) {
    context->case_result->status = status;
    context->case_result->status_valid = 1u;
  }
  h2_pal_result_t close_rc = disconnect_transport(context);
  return rc == H2_PAL_OK ? close_rc : rc;
}

static h2_pal_result_t
run_legacy_commands_absent(h2_e2e_transport_context_t *context) {
  static const char expected_help[] =
      "h2loader <help|status|stats|memory|wifi|stage|reboot "
      "app|loader|upgrade|coredump>\n";
  h2_pal_result_t rc =
      run_simple_command(context, H2_H2LOADER_HOST_COMMAND_HELP);
  if (rc == H2_PAL_OK &&
      (!context->authoritative_command_availability_valid ||
       (context->authoritative_command_availability &
        H2_E2E_REMOVED_COMMAND_AVAILABILITY_MASK) != 0u ||
       strcmp(context->command_output, expected_help) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  return rc;
}

static h2_pal_result_t run_coredump_status(h2_e2e_transport_context_t *context,
                                           uint64_t expected_bytes,
                                           int expected_blank) {
  h2_pal_result_t rc =
      run_simple_command(context, H2_H2LOADER_HOST_COMMAND_COREDUMP_STATUS);
  uint64_t actual_bytes = 0u;
  int actual_blank = -1;
  if (rc == H2_PAL_OK &&
      (!parse_coredump_status(context->command_output, &actual_bytes,
                              &actual_blank) ||
       actual_bytes != expected_bytes || actual_blank != expected_blank)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  context->case_result->acknowledged_bytes = actual_bytes;
  context->case_result->total_bytes = expected_bytes;
  return rc;
}

static h2_pal_result_t run_coredump_dump(h2_e2e_transport_context_t *context) {
  h2_h2loader_host_status_t status;
  h2_h2loader_host_command_result_t result = {0};
  h2_pal_result_t rc = connect_transport(context, &status);
  if (rc == H2_PAL_OK) {
    const h2_h2loader_host_command_request_t request = {
        .command = H2_H2LOADER_HOST_COMMAND_COREDUMP_DUMP,
        .status = &status,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = coredump_output,
        .output_user = context,
    };
    rc = execute_command(context, &request, &result);
    context->case_result->terminal = result.terminal;
    if (rc == H2_PAL_OK &&
        (result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK ||
         context->coredump_line_size != 0u ||
         context->coredump_terminal_seen == 0u ||
         context->coredump_terminal_bytes !=
             context->expected_coredump_bytes)) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
  }
  if (rc == H2_PAL_OK)
    rc = read_status(context, &status);
  if (rc == H2_PAL_OK) {
    context->case_result->status = status;
    context->case_result->status_valid = 1u;
  }
  context->case_result->acknowledged_bytes = context->coredump_decoded_bytes;
  context->case_result->total_bytes = context->expected_coredump_bytes;
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
    if (rc == H2_PAL_OK)
      rc = read_status(context, &status);
    if (rc == H2_PAL_OK) {
      context->case_result->status = status;
      context->case_result->status_valid = 1u;
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

static h2_pal_result_t
run_stage_payload(h2_e2e_transport_context_t *context,
                  const h2_h2loader_host_catalog_entry_t *asset,
                  const uint8_t *firmware, size_t firmware_size) {
  const h2_e2e_memory_source_t source = {
      .data = firmware,
      .size = firmware_size,
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
      .asset = asset,
      .read_payload = memory_read,
      .payload_user = (void *)&source,
      .is_cancelled = context->config->is_cancelled,
      .cancel_user = context->config->cancel_user,
      .on_progress = record_progress,
      .progress_user = context,
      .reconnect_delay_ms = 250u,
      .reconnect_attempts = 240u,
  };
  context->case_result->total_bytes = asset->bytes;
  h2_pal_result_t rc =
      h2_h2loader_host_stage_operation_run(&operation, &final_status);
  if (rc == H2_PAL_OK) {
    context->case_result->status = final_status;
    context->case_result->status_valid = 1u;
  }
  return rc;
}

static h2_pal_result_t run_send(h2_e2e_transport_context_t *context) {
  return run_stage_payload(context, &context->app_asset,
                           context->config->app_firmware,
                           context->config->app_firmware_size);
}

static h2_pal_result_t run_send_url(h2_e2e_transport_context_t *context) {
  h2_h2loader_host_status_t initial_status;
  h2_h2loader_host_status_t final_status = {0};
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

static h2_pal_result_t
reconnect_after_reboot(h2_e2e_transport_context_t *context,
                       uint32_t expected_partition,
                       h2_h2loader_host_status_t *out_status) {
  if (context->transport == H2_H2LOADER_E2E_TRANSPORT_BLE &&
      !context->authoritative_device_uid_valid) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_pal_result_t rc = H2_PAL_ERR_TIMEOUT;
  for (uint32_t attempt = 0u; attempt < 120u && !cancelled(context->config);
       ++attempt) {
    ++context->case_result->reconnect_attempts;
    rc = h2_pal_time_sleep_ms(context->config->runtime->time, 500u);
    if (rc != H2_PAL_OK)
      return rc;
    rc = connect_transport(context, out_status);
    if (rc == H2_PAL_ERR_INVALID_STATE)
      return rc;
    if (rc == H2_PAL_OK &&
        out_status->running_partition == expected_partition) {
      return H2_PAL_OK;
    }
    (void)disconnect_transport(context);
  }
  return cancelled(context->config) ? H2_PAL_EXIT : rc;
}

static h2_pal_result_t run_reboot_monitor(h2_e2e_transport_context_t *context,
                                          h2_h2loader_host_command_t command,
                                          uint32_t expected_partition) {
  h2_h2loader_host_status_t status;
  h2_h2loader_host_command_result_t result = {0};
  context->monitor_logs = 1u;
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
  (void)disconnect_transport(context);
  /* A reboot ACK may precede the actual reset (for example, the vendor's
   * deferred-reset timer). The first reconnect can still reach the old
   * instance. An external USB-UART adapter stays open across a CPU reset:
   * logs continue, but the old KCP conversation no longer exists on the MCU.
   * Allow one CLOSED/TIMEOUT transition (including the post-monitor status),
   * then require a new session, full monitor window and live status. A second
   * failure is terminal, so repeated resets are not hidden by retries. */
  for (unsigned transition = 0u; rc == H2_PAL_OK && transition < 2u;
       ++transition) {
    context->monitor_output_bytes = 0u;
    rc = reconnect_after_reboot(context, expected_partition, &status);
    if (rc == H2_PAL_OK) rc = begin_monitor_window(context);
    if (rc == H2_PAL_OK) rc = finish_bounded_monitor(context, 1);
    if (rc == H2_PAL_OK) rc = read_status(context, &status);
    if ((rc == H2_PAL_ERR_CLOSED || rc == H2_PAL_ERR_TIMEOUT) &&
        transition == 0u &&
        !cancelled(context->config)) {
      (void)disconnect_transport(context);
      rc = H2_PAL_OK;
      continue;
    }
    if (rc == H2_PAL_OK && status.running_partition != expected_partition)
      rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK) {
      context->case_result->status = status;
      context->case_result->status_valid = 1u;
    }
    break;
  }
  h2_pal_result_t close_rc = disconnect_transport(context);
  context->monitor_logs = 0u;
  return rc == H2_PAL_OK ? close_rc : rc;
}

static h2_pal_result_t run_reboot_preserves_stage(
    h2_e2e_transport_context_t *context,
    h2_h2loader_host_command_t reboot_command, uint32_t expected_partition,
    const h2_h2loader_host_catalog_entry_t *stage_asset,
    const uint8_t *stage_firmware, size_t stage_firmware_size) {
  h2_h2loader_host_status_t status;
  h2_h2loader_host_command_result_t result = {0};
  h2_pal_result_t rc = run_stage_payload(context, stage_asset, stage_firmware,
                                         stage_firmware_size);
  if (rc != H2_PAL_OK)
    return rc;
  rc = connect_transport(context, &status);
  if (rc == H2_PAL_OK &&
      (!status.stage.valid ||
       strcmp(status.stage.package_checksum, stage_asset->sha256) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    const h2_h2loader_host_command_request_t request = {
        .command = reboot_command,
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
  (void)disconnect_transport(context);
  if (rc == H2_PAL_OK) {
    rc = reconnect_after_reboot(context, expected_partition, &status);
  }
  if (rc == H2_PAL_OK &&
      (!status.stage.valid ||
       strcmp(status.stage.package_checksum, stage_asset->sha256) != 0 ||
       status.stage.package_size != stage_asset->bytes)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    const h2_h2loader_host_command_request_t abort_request = {
        .command = H2_H2LOADER_HOST_COMMAND_STAGE_ABORT,
        .status = &status,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = count_output,
        .output_user = context,
    };
    memset(&result, 0, sizeof(result));
    rc = execute_command(context, &abort_request, &result);
    if (rc == H2_PAL_OK &&
        result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
      rc = H2_PAL_ERR_IO;
    }
  }
  if (rc == H2_PAL_OK) {
    rc = read_status(context, &status);
  }
  if (rc == H2_PAL_OK && status.stage.valid) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    context->case_result->status = status;
    context->case_result->status_valid = 1u;
  }
  h2_pal_result_t close_rc = disconnect_transport(context);
  return rc == H2_PAL_OK ? close_rc : rc;
}

static int metadata_same_image(const h2_h2loader_host_metadata_t *a,
                               const h2_h2loader_host_metadata_t *b) {
  return a->valid && b->valid && a->role == b->role &&
         a->image_size == b->image_size &&
         strcmp(a->image_checksum, b->image_checksum) == 0 &&
         strcmp(a->version, b->version) == 0 &&
         strcmp(a->board, b->board) == 0 && strcmp(a->target, b->target) == 0;
}

static int metadata_equal(const h2_h2loader_host_metadata_t *a,
                          const h2_h2loader_host_metadata_t *b) {
  return a->valid == b->valid && a->package_size == b->package_size &&
         a->image_size == b->image_size && a->role == b->role &&
         strcmp(a->package_checksum, b->package_checksum) == 0 &&
         strcmp(a->image_checksum, b->image_checksum) == 0 &&
         strcmp(a->version, b->version) == 0 &&
         strcmp(a->board, b->board) == 0 && strcmp(a->target, b->target) == 0;
}

static int
metadata_matches_asset(const h2_h2loader_host_metadata_t *metadata,
                       const h2_h2loader_host_catalog_entry_t *asset) {
  h2_h2loader_host_active_role_t role =
      asset->role == H2_H2LOADER_HOST_ASSET_ROLE_APP
          ? H2_H2LOADER_HOST_ACTIVE_ROLE_APP
          : H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
  return metadata->valid && metadata->role == role &&
         metadata->package_size == asset->bytes &&
         strcmp(metadata->package_checksum, asset->sha256) == 0 &&
         strcmp(metadata->image_checksum, asset->image_sha256) == 0 &&
         strcmp(metadata->version, asset->version) == 0 &&
         strcmp(metadata->board, asset->board) == 0 &&
         strcmp(metadata->target, asset->target) == 0;
}

static h2_pal_result_t
run_install(h2_e2e_transport_context_t *context,
            const h2_h2loader_host_catalog_entry_t *asset,
            const uint8_t *firmware, size_t firmware_size) {
  const h2_e2e_memory_source_t source = {.data = firmware,
                                         .size = firmware_size};
  static const h2_h2loader_host_managed_transport_vtable_t vtable = {
      .connect = managed_connect,
      .stage = managed_stage,
      .activate = managed_activate,
      .read_status = managed_read_status,
      .disconnect = managed_disconnect,
      .rediscover = managed_rediscover,
  };
  h2_h2loader_host_status_t final_status;
  const h2_h2loader_host_managed_operation_config_t operation = {
      .time = context->config->runtime->time,
      .transport = {.user = context, .vtable = &vtable},
      .asset = asset,
      .read_payload = memory_read,
      .payload_user = (void *)&source,
      .is_cancelled = context->config->is_cancelled,
      .cancel_user = context->config->cancel_user,
      .on_progress = record_progress,
      .progress_user = context,
      .reconnect_delay_ms = 500u,
      .reconnect_attempts = 240u,
  };
  context->case_result->total_bytes = asset->bytes;
  h2_pal_result_t rc =
      h2_h2loader_host_managed_operation_run(&operation, &final_status);
  if (rc == H2_PAL_OK && asset->role == H2_H2LOADER_HOST_ASSET_ROLE_APP &&
      (final_status.running_partition != 2u || final_status.stage.valid)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK && asset->role == H2_H2LOADER_HOST_ASSET_ROLE_LOADER &&
      (final_status.running_partition != 1u || final_status.stage.valid ||
       !metadata_same_image(&final_status.partition_1,
                            &final_status.partition_2) ||
       strcmp(final_status.partition_1.package_checksum, asset->sha256) != 0 ||
       strcmp(final_status.partition_2.package_checksum, asset->sha256) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    context->case_result->status = final_status;
    context->case_result->status_valid = 1u;
    context->case_result->terminal = H2_H2LOADER_HOST_COMMAND_TERMINAL_OK;
  }
  return rc;
}

static h2_pal_result_t
run_install_crash_app(h2_e2e_transport_context_t *context) {
  h2_h2loader_host_status_t status;
  h2_h2loader_host_metadata_t staged_candidate = {0};
  int32_t staged_last = 0;
  h2_h2loader_host_command_result_t result = {0};
  h2_pal_result_t rc = run_stage_payload(context, &context->crash_asset,
                                         context->config->crash_firmware,
                                         context->config->crash_firmware_size);
  if (rc != H2_PAL_OK)
    return rc;
  rc = connect_transport(context, &status);
  if (rc == H2_PAL_OK &&
      !metadata_matches_asset(&status.stage, &context->crash_asset)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    staged_candidate = status.stage;
    staged_last = status.last;
  }
  if (rc == H2_PAL_OK) {
    const h2_h2loader_host_command_request_t request = {
        .command = H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE,
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
  (void)disconnect_transport(context);
  if (rc != H2_PAL_OK)
    return rc;

  rc = H2_PAL_ERR_TIMEOUT;
  for (uint32_t attempt = 0u; attempt < 240u && !cancelled(context->config);
       ++attempt) {
    ++context->case_result->reconnect_attempts;
    rc = h2_pal_time_sleep_ms(context->config->runtime->time, 500u);
    if (rc != H2_PAL_OK)
      return rc;
    rc = connect_transport(context, &status);
    if (rc == H2_PAL_ERR_INVALID_STATE)
      return rc;
    if (rc == H2_PAL_OK && status.running_partition == 1u &&
        metadata_equal(&status.stage, &staged_candidate) &&
        metadata_equal(&status.partition_2, &staged_candidate) &&
        status.boot_intent == H2_H2LOADER_HOST_BOOT_INTENT_AUTO &&
        status.last == staged_last) {
      context->case_result->status = status;
      context->case_result->status_valid = 1u;
      (void)disconnect_transport(context);
      break;
    }
    (void)disconnect_transport(context);
    rc = H2_PAL_ERR_TIMEOUT;
  }
  if (rc != H2_PAL_OK)
    return cancelled(context->config) ? H2_PAL_EXIT : rc;

  context->command_output_size = 0u;
  context->command_output[0] = '\0';
  rc = run_simple_command(context, H2_H2LOADER_HOST_COMMAND_COREDUMP_STATUS);
  uint64_t bytes = 0u;
  int blank = -1;
  if (rc == H2_PAL_OK &&
      (!parse_coredump_status(context->command_output, &bytes, &blank) ||
       blank != 0 || bytes < sizeof(uint32_t))) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK) {
    context->expected_coredump_bytes = bytes;
    context->case_result->acknowledged_bytes = bytes;
    context->case_result->total_bytes = bytes;
  }
  return rc;
}

static h2_pal_result_t
run_install_monitor(h2_e2e_transport_context_t *context,
                    const h2_h2loader_host_catalog_entry_t *asset,
                    const uint8_t *firmware, size_t firmware_size) {
  context->monitor_logs = 1u;
  h2_pal_result_t rc = run_install(context, asset, firmware, firmware_size);
  context->monitor_logs = 0u;
  if (rc == H2_PAL_OK && context->monitor_output_bytes == 0u) {
    rc = H2_PAL_ERR_NOT_FOUND;
  }
  return rc;
}

static h2_pal_result_t require_case_active_role(
    h2_e2e_transport_context_t *context, h2_pal_result_t rc,
    h2_h2loader_host_active_role_t expected_role) {
  if (rc != H2_PAL_OK)
    return rc;

  /* Command helpers normally save their authoritative post-status. Keep the
   * role assertion self-contained if a future helper returns success without
   * doing so. connect_transport reads and records status for both transports. */
  if (!context->case_result->status_valid) {
    h2_h2loader_host_status_t status;
    rc = connect_transport(context, &status);
    h2_pal_result_t close_rc = disconnect_transport(context);
    if (rc == H2_PAL_OK)
      rc = close_rc;
  }
  if (rc == H2_PAL_OK &&
      context->case_result->status.active_role != expected_role) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return rc;
}

static h2_pal_result_t execute_real_case(h2_e2e_transport_context_t *context,
                                         h2_h2loader_e2e_case_t test_case) {
  switch (test_case) {
  case H2_H2LOADER_E2E_CASE_HELP:
    return run_simple_command(context, H2_H2LOADER_HOST_COMMAND_HELP);
  case H2_H2LOADER_E2E_CASE_STATUS:
    return run_status(context);
  case H2_H2LOADER_E2E_CASE_STATS:
    return run_simple_command(context, H2_H2LOADER_HOST_COMMAND_STATS);
  case H2_H2LOADER_E2E_CASE_MEMORY:
    return run_simple_command(context, H2_H2LOADER_HOST_COMMAND_MEMORY);
  case H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT:
    return run_legacy_commands_absent(context);
  case H2_H2LOADER_E2E_CASE_WIFI_SCAN:
    return run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_SCAN);
  case H2_H2LOADER_E2E_CASE_WIFI_CONNECT:
    return run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT);
  case H2_H2LOADER_E2E_CASE_WIFI_DISCONNECT:
    return run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT);
  case H2_H2LOADER_E2E_CASE_APP_HELP:
    return require_case_active_role(
        context, run_simple_command(context, H2_H2LOADER_HOST_COMMAND_HELP),
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_APP_STATUS:
    return require_case_active_role(context, run_status(context),
                                    H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_APP_STATS:
    return require_case_active_role(
        context, run_simple_command(context, H2_H2LOADER_HOST_COMMAND_STATS),
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_APP_MEMORY:
    return require_case_active_role(
        context, run_simple_command(context, H2_H2LOADER_HOST_COMMAND_MEMORY),
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_APP_LEGACY_COMMANDS_ABSENT:
    return require_case_active_role(context, run_legacy_commands_absent(context),
                                    H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_APP_WIFI_SCAN:
    return require_case_active_role(
        context, run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_SCAN),
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_APP_WIFI_CONNECT:
    return require_case_active_role(
        context,
        run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT),
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_APP_WIFI_DISCONNECT:
    return require_case_active_role(
        context,
        run_wifi_command(context, H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT),
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_SEND:
    return require_case_active_role(
        context, run_send(context), H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER);
  case H2_H2LOADER_E2E_CASE_APP_SEND:
    return require_case_active_role(
        context, run_send(context), H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND:
  case H2_H2LOADER_E2E_CASE_STAGE_ABORT_AFTER_SEND_URL:
    return require_case_active_role(
        context,
        run_simple_command(context, H2_H2LOADER_HOST_COMMAND_STAGE_ABORT),
        H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER);
  case H2_H2LOADER_E2E_CASE_APP_STAGE_ABORT_AFTER_SEND:
  case H2_H2LOADER_E2E_CASE_APP_STAGE_ABORT_AFTER_SEND_URL:
    return require_case_active_role(
        context,
        run_simple_command(context, H2_H2LOADER_HOST_COMMAND_STAGE_ABORT),
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_SEND_URL:
    return require_case_active_role(
        context, run_send_url(context), H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER);
  case H2_H2LOADER_E2E_CASE_APP_SEND_URL:
    return require_case_active_role(
        context, run_send_url(context), H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
  case H2_H2LOADER_E2E_CASE_REBOOT_APP_PRESERVES_STAGE:
    return run_reboot_preserves_stage(
        context, H2_H2LOADER_HOST_COMMAND_REBOOT_APP, 2u,
        &context->loader_asset, context->config->loader_firmware,
        context->config->loader_firmware_size);
  case H2_H2LOADER_E2E_CASE_REBOOT_LOADER_PRESERVES_STAGE:
    return run_reboot_preserves_stage(
        context, H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER, 1u,
        &context->app_asset, context->config->app_firmware,
        context->config->app_firmware_size);
  case H2_H2LOADER_E2E_CASE_INSTALL_APP:
    return run_install(context, &context->app_asset,
                       context->config->app_firmware,
                       context->config->app_firmware_size);
  case H2_H2LOADER_E2E_CASE_INSTALL_LOADER:
    return run_install(context, &context->loader_asset,
                       context->config->loader_firmware,
                       context->config->loader_firmware_size);
  case H2_H2LOADER_E2E_CASE_COREDUMP_STATUS:
    return run_coredump_status(context, context->expected_coredump_bytes, 0);
  case H2_H2LOADER_E2E_CASE_COREDUMP_DUMP:
    return run_coredump_dump(context);
  case H2_H2LOADER_E2E_CASE_COREDUMP_ERASE:
    return run_simple_command(context, H2_H2LOADER_HOST_COMMAND_COREDUMP_ERASE);
  case H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE:
    return run_coredump_status(context, 0u, 1);
  case H2_H2LOADER_E2E_CASE_MONITOR:
    return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
               ? run_monitor(context)
               : H2_PAL_ERR_UNSUPPORTED;
  case H2_H2LOADER_E2E_CASE_REBOOT_LOADER_MONITOR:
    return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
               ? run_reboot_monitor(context,
                                    H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER, 1u)
               : H2_PAL_ERR_UNSUPPORTED;
  case H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR:
    return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
               ? run_reboot_monitor(context,
                                    H2_H2LOADER_HOST_COMMAND_REBOOT_APP, 2u)
               : H2_PAL_ERR_UNSUPPORTED;
  case H2_H2LOADER_E2E_CASE_REBOOT_UPGRADE_MONITOR:
    return context->transport == H2_H2LOADER_E2E_TRANSPORT_UART
               ? run_install_monitor(context, &context->app_asset,
                                     context->config->app_firmware,
                                     context->config->app_firmware_size)
               : H2_PAL_ERR_UNSUPPORTED;
  case H2_H2LOADER_E2E_CASE_INSTALL_CRASH_APP:
    return run_install_crash_app(context);
  default:
    return H2_PAL_ERR_INVALID_ARG;
  }
}

static h2_pal_result_t
inspect_firmware(const h2_h2loader_e2e_config_t *config,
                 const uint8_t *firmware, size_t firmware_size,
                 h2_h2loader_host_catalog_entry_t *out_asset) {
  const h2_e2e_memory_source_t source = {
      .data = firmware,
      .size = firmware_size,
  };
  const h2_h2loader_host_package_inspect_config_t inspect = {
      .allocator = config->runtime->mem,
      .read_payload = memory_read,
      .payload_user = (void *)&source,
      .payload_bytes = firmware_size,
  };
  return h2_h2loader_host_package_inspect(&inspect, out_asset);
}

static size_t cases_per_transport(const h2_h2loader_e2e_config_t *config) {
  size_t count = 5u;
  if (config->include_wifi)
    count += 3u;
  if (config->include_send)
    count += 2u;
  if (config->include_send_url)
    count += 2u;
  if (config->include_lifecycle)
    count += 4u;
  if (config->include_lifecycle)
    count += 5u;
  if (config->include_lifecycle && config->include_wifi)
    count += 3u;
  if (config->include_lifecycle && config->include_send)
    count += 2u;
  if (config->include_lifecycle && config->include_send_url)
    count += 2u;
  if (config->include_coredump)
    count += 4u;
  return count;
}

static size_t
total_cases_per_iteration(const h2_h2loader_e2e_config_t *config) {
  const size_t transports = (config->uart_endpoint != NULL ? 1u : 0u) +
                            (config->ble_endpoint != NULL ? 1u : 0u);
  return cases_per_transport(config) * transports +
         (config->include_monitor ? 3u : 0u) +
         (config->include_crash ? 1u : 0u);
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
      (config->app_firmware == NULL || config->app_firmware_size == 0u)) {
    return 0;
  }
  if (config->include_lifecycle &&
      (config->app_firmware == NULL || config->app_firmware_size == 0u ||
       config->loader_firmware == NULL || config->loader_firmware_size == 0u)) {
    return 0;
  }
  if (config->include_send_url &&
      (config->firmware_url == NULL || config->firmware_url[0] == '\0' ||
       config->firmware_url_bytes == 0u ||
       !sha256_valid(config->firmware_url_sha256))) {
    return 0;
  }
  if (config->include_coredump &&
      ((!config->include_crash &&
        config->expected_coredump_bytes < sizeof(uint32_t)) ||
       config->repeat_count != 1u)) {
    return 0;
  }
  if (config->include_crash &&
      (!config->include_coredump || config->crash_firmware == NULL ||
       config->crash_firmware_size == 0u || config->repeat_count != 1u)) {
    return 0;
  }
  if (config->include_monitor &&
      (config->uart_endpoint == NULL || config->monitor_duration_ms == 0u)) {
    return 0;
  }
  return total_cases_per_iteration(config) * config->repeat_count <=
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
  context->coredump_line_size = 0u;
  context->coredump_decoded_bytes = 0u;
  context->coredump_terminal_bytes = 0u;
  context->coredump_terminal_seen = 0u;
  context->monitor_output_bytes = 0u;
  if (config->on_case != NULL)
    config->on_case(config->case_user, entry, 1);
  uint64_t started = 0u;
  uint64_t finished = 0u;
  (void)now_ms(config, &started);
  entry->result = config->execute_case != NULL
                      ? config->execute_case(config->execute_user, transport,
                                             test_case, entry)
                      : execute_real_case(context, test_case);
  if (entry->result == H2_PAL_OK &&
      test_case == H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT &&
      (!context->authoritative_command_availability_valid ||
       (context->authoritative_command_availability &
        H2_E2E_REMOVED_COMMAND_AVAILABILITY_MASK) != 0u)) {
    entry->result = H2_PAL_ERR_INVALID_STATE;
  }
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
  context->authoritative_command_availability = 0u;
  context->authoritative_command_availability_valid = 0u;
  append_case(config, result, context, transport, H2_H2LOADER_E2E_CASE_HELP,
              iteration);
  const size_t status_case_index = result->case_count;
  append_case(config, result, context, transport, H2_H2LOADER_E2E_CASE_STATUS,
              iteration);
  append_case(config, result, context, transport, H2_H2LOADER_E2E_CASE_STATS,
              iteration);
  const h2_h2loader_e2e_case_result_t *status_case =
      &result->cases[status_case_index];
  if (status_case->result == H2_PAL_OK && status_case->status_valid) {
    context->authoritative_command_availability =
        status_case->status.command_availability;
    context->authoritative_command_availability_valid = 1u;
  }
  if (status_case->result == H2_PAL_OK && status_case->status_valid &&
      (status_case->status.command_availability &
       H2_H2LOADER_HOST_COMMAND_AVAILABLE_MEMORY) != 0u) {
    append_case(config, result, context, transport, H2_H2LOADER_E2E_CASE_MEMORY,
                iteration);
  }
  append_case(config, result, context, transport,
              H2_H2LOADER_E2E_CASE_LEGACY_COMMANDS_ABSENT, iteration);
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
  if (config->include_monitor && transport == H2_H2LOADER_E2E_TRANSPORT_UART) {
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_MONITOR, iteration);
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_REBOOT_LOADER_MONITOR, iteration);
    if (!config->include_lifecycle) {
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR, iteration);
    }
  }
  if (config->include_lifecycle) {
    append_case(config, result, context, transport,
                config->include_monitor &&
                        transport == H2_H2LOADER_E2E_TRANSPORT_UART
                    ? H2_H2LOADER_E2E_CASE_REBOOT_UPGRADE_MONITOR
                    : H2_H2LOADER_E2E_CASE_INSTALL_APP,
                iteration);
    if (config->include_monitor &&
        transport == H2_H2LOADER_E2E_TRANSPORT_UART) {
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_REBOOT_APP_MONITOR, iteration);
    }
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_APP_HELP, iteration);
    const size_t app_status_case_index = result->case_count;
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_APP_STATUS, iteration);
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_APP_STATS, iteration);
    const h2_h2loader_e2e_case_result_t *app_status_case =
        &result->cases[app_status_case_index];
    if (app_status_case->result == H2_PAL_OK &&
        app_status_case->status_valid) {
      context->authoritative_command_availability =
          app_status_case->status.command_availability;
      context->authoritative_command_availability_valid = 1u;
    }
    if (app_status_case->result == H2_PAL_OK &&
        app_status_case->status_valid &&
        (app_status_case->status.command_availability &
         H2_H2LOADER_HOST_COMMAND_AVAILABLE_MEMORY) != 0u) {
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_APP_MEMORY, iteration);
    }
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_APP_LEGACY_COMMANDS_ABSENT, iteration);
    if (config->include_wifi) {
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_APP_WIFI_SCAN, iteration);
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_APP_WIFI_CONNECT, iteration);
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_APP_WIFI_DISCONNECT, iteration);
    }
    if (config->include_send) {
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_APP_SEND, iteration);
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_APP_STAGE_ABORT_AFTER_SEND, iteration);
    }
    if (config->include_send_url) {
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_APP_SEND_URL, iteration);
      append_case(config, result, context, transport,
                  H2_H2LOADER_E2E_CASE_APP_STAGE_ABORT_AFTER_SEND_URL,
                  iteration);
    }
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_REBOOT_APP_PRESERVES_STAGE, iteration);
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_REBOOT_LOADER_PRESERVES_STAGE, iteration);
    append_case(config, result, context, transport,
                H2_H2LOADER_E2E_CASE_INSTALL_LOADER, iteration);
  }
}

static void run_coredump_read_cases(const h2_h2loader_e2e_config_t *config,
                                    h2_h2loader_e2e_result_t *result,
                                    h2_e2e_transport_context_t *context,
                                    h2_h2loader_e2e_transport_t transport,
                                    uint32_t iteration) {
  context->transport = transport;
  append_case(config, result, context, transport,
              H2_H2LOADER_E2E_CASE_COREDUMP_STATUS, iteration);
  append_case(config, result, context, transport,
              H2_H2LOADER_E2E_CASE_COREDUMP_DUMP, iteration);
}

static void run_coredump_cleanup_cases(const h2_h2loader_e2e_config_t *config,
                                       h2_h2loader_e2e_result_t *result,
                                       h2_e2e_transport_context_t *context,
                                       h2_h2loader_e2e_transport_t transport,
                                       uint32_t iteration) {
  context->transport = transport;
  append_case(config, result, context, transport,
              H2_H2LOADER_E2E_CASE_COREDUMP_ERASE, iteration);
  append_case(config, result, context, transport,
              H2_H2LOADER_E2E_CASE_COREDUMP_STATUS_AFTER_ERASE, iteration);
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
  if ((config->include_send || config->include_lifecycle) &&
      config->execute_case == NULL) {
    h2_pal_result_t rc =
        inspect_firmware(config, config->app_firmware,
                         config->app_firmware_size, &context.app_asset);
    if (rc != H2_PAL_OK) {
      out_result->result = rc;
      out_result->complete = 1;
      return rc;
    }
    if (context.app_asset.role != H2_H2LOADER_HOST_ASSET_ROLE_APP) {
      out_result->result = H2_PAL_ERR_FORMAT;
      out_result->complete = 1;
      return out_result->result;
    }
    out_result->app_firmware_bytes = context.app_asset.bytes;
    memcpy(out_result->app_firmware_sha256, context.app_asset.sha256,
           sizeof(out_result->app_firmware_sha256));
  }
  if (config->include_lifecycle && config->execute_case == NULL) {
    h2_pal_result_t rc =
        inspect_firmware(config, config->loader_firmware,
                         config->loader_firmware_size, &context.loader_asset);
    if (rc != H2_PAL_OK ||
        context.loader_asset.role != H2_H2LOADER_HOST_ASSET_ROLE_LOADER) {
      out_result->result = rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc;
      out_result->complete = 1;
      return out_result->result;
    }
    out_result->loader_firmware_bytes = context.loader_asset.bytes;
    memcpy(out_result->loader_firmware_sha256, context.loader_asset.sha256,
           sizeof(out_result->loader_firmware_sha256));
  }
  if (config->include_crash && config->execute_case == NULL) {
    h2_pal_result_t rc =
        inspect_firmware(config, config->crash_firmware,
                         config->crash_firmware_size, &context.crash_asset);
    if (rc != H2_PAL_OK ||
        context.crash_asset.role != H2_H2LOADER_HOST_ASSET_ROLE_APP) {
      out_result->result = rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc;
      out_result->complete = 1;
      return out_result->result;
    }
    out_result->crash_firmware_bytes = context.crash_asset.bytes;
    memcpy(out_result->crash_firmware_sha256, context.crash_asset.sha256,
           sizeof(out_result->crash_firmware_sha256));
  }
  context.expected_coredump_bytes = config->expected_coredump_bytes;
  out_result->coredump_bytes = context.expected_coredump_bytes;
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
    if (config->include_crash && !cancelled(config)) {
      append_case(config, out_result, &context,
                  config->uart_endpoint != NULL ? H2_H2LOADER_E2E_TRANSPORT_UART
                                                : H2_H2LOADER_E2E_TRANSPORT_BLE,
                  H2_H2LOADER_E2E_CASE_INSTALL_CRASH_APP, iteration);
      out_result->coredump_bytes = context.expected_coredump_bytes;
    }
    if (config->include_coredump && !cancelled(config)) {
      if (config->uart_endpoint != NULL) {
        run_coredump_read_cases(config, out_result, &context,
                                H2_H2LOADER_E2E_TRANSPORT_UART, iteration);
      }
      if (config->ble_endpoint != NULL && !cancelled(config)) {
        run_coredump_read_cases(config, out_result, &context,
                                H2_H2LOADER_E2E_TRANSPORT_BLE, iteration);
      }
      if (config->uart_endpoint != NULL && !cancelled(config)) {
        run_coredump_cleanup_cases(config, out_result, &context,
                                   H2_H2LOADER_E2E_TRANSPORT_UART, iteration);
      }
      if (config->ble_endpoint != NULL && !cancelled(config)) {
        run_coredump_cleanup_cases(config, out_result, &context,
                                   H2_H2LOADER_E2E_TRANSPORT_BLE, iteration);
      }
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
