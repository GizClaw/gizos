#include "h2_h2loader_serial_e2e.h"

#include <string.h>

typedef struct h2_h2loader_serial_e2e_context {
  h2_runtime_t *runtime;
  const h2_h2loader_serial_e2e_config_t *config;
  h2_h2loader_serial_e2e_result_t *result;
  h2_h2loader_host_serial_connection_t *connection;
  const char *payload_resource_name;
  int initial_status_recorded;
} h2_h2loader_serial_e2e_context_t;

static uint32_t h2_h2loader_serial_e2e_timeout(uint32_t value,
                                               uint32_t fallback) {
  return value == 0u ? fallback : value;
}

static int h2_h2loader_serial_e2e_text_matches(const char *expected,
                                                const char *actual) {
  return expected == NULL || expected[0] == '\0' ||
         (actual != NULL && strcmp(expected, actual) == 0);
}

static int h2_h2loader_serial_e2e_sha256_valid(const char *sha256) {
  if (sha256 == NULL ||
      strlen(sha256) != H2_H2LOADER_HOST_SHA256_HEX_LEN) {
    return 0;
  }
  for (size_t index = 0u; index < H2_H2LOADER_HOST_SHA256_HEX_LEN; ++index) {
    const char value = sha256[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

static int h2_h2loader_serial_e2e_command_valid(
    h2_h2loader_host_command_t command) {
  return command == 0 || command == H2_H2LOADER_HOST_COMMAND_HELP ||
         command == H2_H2LOADER_HOST_COMMAND_STATUS ||
         command == H2_H2LOADER_HOST_COMMAND_STATS ||
         command == H2_H2LOADER_HOST_COMMAND_MEMORY;
}

static void h2_h2loader_serial_e2e_record(
    h2_h2loader_serial_e2e_result_t *result,
    h2_h2loader_serial_e2e_case_id_t case_id, h2_pal_result_t case_result) {
  if (result->case_count < H2_H2LOADER_SERIAL_E2E_MAX_CASES) {
    result->cases[result->case_count++] =
        (h2_h2loader_serial_e2e_case_result_t){case_id, case_result};
  }
  ++result->selected;
  if (case_result == H2_PAL_OK) {
    ++result->passed;
  } else {
    ++result->failed;
    if (result->result == H2_PAL_OK) result->result = case_result;
  }
}

static void h2_h2loader_serial_e2e_skip(
    h2_h2loader_serial_e2e_result_t *result,
    h2_h2loader_serial_e2e_case_id_t case_id) {
  if (result->case_count < H2_H2LOADER_SERIAL_E2E_MAX_CASES) {
    result->cases[result->case_count++] =
        (h2_h2loader_serial_e2e_case_result_t){
            case_id, H2_PAL_ERR_UNAVAILABLE};
  }
  ++result->selected;
  ++result->skipped;
}

static h2_pal_result_t h2_h2loader_serial_e2e_scan(
    h2_h2loader_serial_e2e_context_t *context, int require_selected) {
  h2_h2loader_host_candidate_t candidates[32] = {{0}};
  h2_h2loader_host_scan_result_t scan = {0};
  const h2_h2loader_host_scan_config_t scan_config = {
      .serial = context->config->serial,
      .candidates = candidates,
      .candidate_capacity = sizeof(candidates) / sizeof(candidates[0]),
  };
  h2_pal_result_t result = h2_h2loader_host_scan(&scan_config, &scan);
  context->result->enumerated_ports = scan.required_capacity;
  if (result != H2_PAL_OK) return result;
  if (!require_selected && (context->config->port_id == NULL ||
                            context->config->port_id[0] == '\0')) {
    return H2_PAL_OK;
  }
  if (context->config->port_id == NULL || context->config->port_id[0] == '\0') {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (size_t index = 0u; index < scan.count; ++index) {
    if (strcmp(candidates[index].port_id, context->config->port_id) == 0) {
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t h2_h2loader_serial_e2e_connect(
    h2_h2loader_serial_e2e_context_t *context,
    h2_h2loader_host_status_t *out_status) {
  const h2_h2loader_host_serial_connection_config_t connect_config = {
      .serial = context->config->serial,
      .time = context->runtime->time,
      .allocator = context->runtime->mem,
      .port_id = context->config->port_id,
      .handshake_timeout_ms = h2_h2loader_serial_e2e_timeout(
          context->config->handshake_timeout_ms, 5000u),
      .command_timeout_ms = h2_h2loader_serial_e2e_timeout(
          context->config->command_timeout_ms,
          H2_H2LOADER_HOST_DEFAULT_COMMAND_TIMEOUT_MS),
  };
  h2_pal_result_t result = h2_h2loader_host_serial_connect(
      &connect_config, &context->connection);
  if (result == H2_PAL_OK) {
    result = h2_h2loader_host_serial_read_status(context->connection,
                                                  out_status);
  }
  if (result != H2_PAL_OK) {
    (void)h2_h2loader_host_serial_disconnect(&context->connection);
  }
  if (result == H2_PAL_OK &&
      (!h2_h2loader_serial_e2e_text_matches(context->config->expected_board,
                                             out_status->board) ||
       !h2_h2loader_serial_e2e_text_matches(context->config->expected_target,
                                             out_status->target))) {
    (void)h2_h2loader_host_serial_disconnect(&context->connection);
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (result == H2_PAL_OK && !context->initial_status_recorded) {
    context->result->initial_status = *out_status;
    context->initial_status_recorded = 1;
  }
  return result;
}

static h2_pal_result_t h2_h2loader_serial_e2e_disconnect(
    h2_h2loader_serial_e2e_context_t *context) {
  const h2_pal_result_t result =
      h2_h2loader_host_serial_disconnect(&context->connection);
  if (result != H2_PAL_OK && context->result->cleanup_result == H2_PAL_OK) {
    context->result->cleanup_result = result;
  }
  return result;
}

static h2_pal_result_t h2_h2loader_serial_e2e_status(
    h2_h2loader_serial_e2e_context_t *context) {
  h2_pal_result_t result =
      h2_h2loader_serial_e2e_connect(context, &context->result->final_status);
  const h2_pal_result_t cleanup = h2_h2loader_serial_e2e_disconnect(context);
  return result == H2_PAL_OK ? cleanup : result;
}

static h2_pal_result_t h2_h2loader_serial_e2e_output(
    void *user, const uint8_t *data, size_t len) {
  (void)data;
  ((h2_h2loader_serial_e2e_result_t *)user)->command_output_bytes += len;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_h2loader_serial_e2e_command(
    h2_h2loader_serial_e2e_context_t *context) {
  h2_pal_result_t result =
      h2_h2loader_serial_e2e_connect(context, &context->result->final_status);
  h2_h2loader_host_command_result_t command_result = {0};
  if (result == H2_PAL_OK) {
    const h2_h2loader_host_command_t command = context->config->command == 0
        ? H2_H2LOADER_HOST_COMMAND_STATUS : context->config->command;
    const h2_h2loader_host_command_request_t request = {
        .command = command,
        .status = &context->result->final_status,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = h2_h2loader_serial_e2e_output,
        .output_user = context->result,
    };
    result = h2_h2loader_host_serial_execute_command(
        context->connection, &request, &command_result);
    context->result->command_transport_result =
        command_result.transport_result;
    context->result->command_terminal = command_result.terminal;
    context->result->command_output_bytes = command_result.output_bytes;
    context->result->command_output_truncated =
        command_result.output_truncated;
    context->result->command_lifecycle_transition =
        command_result.lifecycle_transition;
  }
  const h2_pal_result_t cleanup = h2_h2loader_serial_e2e_disconnect(context);
  return result == H2_PAL_OK ? cleanup : result;
}

static void h2_h2loader_serial_e2e_progress(void *user, uint64_t acknowledged,
                                             uint64_t total) {
  h2_h2loader_serial_e2e_result_t *result = user;
  result->acknowledged_bytes = acknowledged;
  result->total_bytes = total;
}

static h2_pal_result_t h2_h2loader_serial_e2e_managed_connect(
    void *user, h2_h2loader_host_status_t *out_status) {
  return h2_h2loader_serial_e2e_connect(user, out_status);
}

static h2_pal_result_t h2_h2loader_serial_e2e_managed_stage(
    void *user, const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload, void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled, void *cancel_user,
    h2_h2loader_host_progress_fn on_progress, void *progress_user) {
  h2_h2loader_serial_e2e_context_t *context = user;
  return h2_h2loader_host_serial_stage(
      context->connection, asset, read_payload, payload_user, is_cancelled,
      cancel_user, on_progress, progress_user);
}

static h2_pal_result_t h2_h2loader_serial_e2e_managed_activate(
    void *user, const h2_h2loader_host_catalog_entry_t *asset) {
  h2_h2loader_serial_e2e_context_t *context = user;
  return h2_h2loader_host_serial_activate(context->connection, asset);
}

static h2_pal_result_t h2_h2loader_serial_e2e_managed_disconnect(void *user) {
  return h2_h2loader_serial_e2e_disconnect(user);
}

static h2_pal_result_t h2_h2loader_serial_e2e_managed_rediscover(void *user) {
  return h2_h2loader_serial_e2e_scan(user, 1);
}

static h2_pal_result_t h2_h2loader_serial_e2e_payload(
    void *user, uint64_t offset, uint8_t *out, size_t out_size,
    size_t *out_read) {
  h2_h2loader_serial_e2e_context_t *context = user;
  return context->config->read_resource(
      context->config->resource_user, context->payload_resource_name,
      offset, out, out_size, out_read);
}

static h2_pal_result_t h2_h2loader_serial_e2e_install(
    h2_h2loader_serial_e2e_context_t *context) {
  if (context->config->catalog_json == NULL ||
      context->config->catalog_json_len == 0u ||
      !h2_h2loader_serial_e2e_sha256_valid(
          context->config->asset_sha256) ||
      context->config->read_resource == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_h2loader_host_catalog_t *catalog = NULL;
  const h2_h2loader_host_catalog_config_t catalog_config = {
      .allocator = context->runtime->mem,
      .index_json = context->config->catalog_json,
      .index_json_len = context->config->catalog_json_len,
      .read_resource = context->config->read_resource,
      .resource_user = context->config->resource_user,
  };
  h2_pal_result_t result = h2_h2loader_host_catalog_open(&catalog_config,
                                                          &catalog);
  size_t count = 0u;
  h2_h2loader_host_catalog_entry_t asset = {0};
  size_t matches = 0u;
  if (result == H2_PAL_OK) result = h2_h2loader_host_catalog_count(catalog, &count);
  for (size_t index = 0u; result == H2_PAL_OK && index < count; ++index) {
    h2_h2loader_host_catalog_entry_t candidate = {0};
    result = h2_h2loader_host_catalog_get(catalog, index, &candidate);
    if (result == H2_PAL_OK && strcmp(candidate.sha256,
                                      context->config->asset_sha256) == 0 &&
        candidate.operation == H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL &&
        h2_h2loader_serial_e2e_text_matches(context->config->expected_board,
                                             candidate.board) &&
        h2_h2loader_serial_e2e_text_matches(context->config->expected_target,
                                             candidate.target)) {
      if (matches == 0u) asset = candidate;
      ++matches;
    }
  }
  if (result == H2_PAL_OK && matches == 0u) result = H2_PAL_ERR_NOT_FOUND;
  if (result == H2_PAL_OK && matches > 1u) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  static const h2_h2loader_host_managed_transport_vtable_t vtable = {
      .connect = h2_h2loader_serial_e2e_managed_connect,
      .stage = h2_h2loader_serial_e2e_managed_stage,
      .activate = h2_h2loader_serial_e2e_managed_activate,
      .disconnect = h2_h2loader_serial_e2e_managed_disconnect,
      .rediscover = h2_h2loader_serial_e2e_managed_rediscover,
  };
  if (result == H2_PAL_OK) {
    context->payload_resource_name = asset.resource_name;
    const h2_h2loader_host_managed_operation_config_t operation = {
        .time = context->runtime->time,
        .transport = {context, &vtable},
        .asset = &asset,
        .read_payload = h2_h2loader_serial_e2e_payload,
        .payload_user = context,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_progress = h2_h2loader_serial_e2e_progress,
        .progress_user = context->result,
        .reconnect_delay_ms = h2_h2loader_serial_e2e_timeout(
            context->config->reconnect_delay_ms, 1000u),
        .reconnect_attempts = h2_h2loader_serial_e2e_timeout(
            context->config->reconnect_attempts, 30u),
    };
    result = h2_h2loader_host_managed_operation_run(
        &operation, &context->result->final_status);
  }
  const h2_pal_result_t cleanup = h2_h2loader_host_catalog_close(&catalog);
  if (cleanup != H2_PAL_OK && context->result->cleanup_result == H2_PAL_OK) {
    context->result->cleanup_result = cleanup;
  }
  return result == H2_PAL_OK ? cleanup : result;
}

h2_pal_result_t h2_h2loader_serial_e2e_run(
    h2_runtime_t *runtime, const h2_h2loader_serial_e2e_config_t *config,
    h2_h2loader_serial_e2e_result_t *out_result) {
  if (out_result != NULL) memset(out_result, 0, sizeof(*out_result));
  if (runtime == NULL || config == NULL || out_result == NULL ||
      config->serial == NULL || runtime->mem == NULL || runtime->time == NULL ||
      config->suite_mask == 0u ||
      (config->suite_mask & ~(H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT |
                              H2_H2LOADER_SERIAL_E2E_SUITE_STATUS |
                              H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND |
                              H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL)) != 0u) {
    if (out_result != NULL) {
      out_result->result = H2_PAL_ERR_INVALID_ARG;
      out_result->complete = 1;
    }
    return H2_PAL_ERR_INVALID_ARG;
  }
  const uint32_t live_suites = H2_H2LOADER_SERIAL_E2E_SUITE_STATUS |
                               H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND |
                               H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL;
  if (((config->suite_mask & live_suites) != 0u &&
       (config->port_id == NULL || config->port_id[0] == '\0')) ||
      ((config->suite_mask & H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND) != 0u &&
       !h2_h2loader_serial_e2e_command_valid(config->command)) ||
      ((config->suite_mask & H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL) != 0u &&
       (config->expected_board == NULL || config->expected_board[0] == '\0' ||
        config->expected_target == NULL ||
        config->expected_target[0] == '\0'))) {
    out_result->result = H2_PAL_ERR_INVALID_ARG;
    out_result->complete = 1;
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_result->result = H2_PAL_OK;
  out_result->cleanup_result = H2_PAL_OK;
  h2_h2loader_serial_e2e_context_t context = {
      .runtime = runtime,
      .config = config,
      .result = out_result,
  };
  uint64_t started = 0u;
  (void)h2_pal_time_get_monotonic_ms(runtime->time, &started);
  int dependent_blocked = 0;
  if ((config->suite_mask & H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT) != 0u) {
    const h2_pal_result_t case_result = h2_h2loader_serial_e2e_scan(&context, 0);
    h2_h2loader_serial_e2e_record(out_result,
        H2_H2LOADER_SERIAL_E2E_CASE_PREFLIGHT,
        case_result);
    dependent_blocked = case_result != H2_PAL_OK;
  }
  if ((config->suite_mask & H2_H2LOADER_SERIAL_E2E_SUITE_STATUS) != 0u) {
    if (dependent_blocked) {
      h2_h2loader_serial_e2e_skip(out_result,
                                  H2_H2LOADER_SERIAL_E2E_CASE_STATUS);
    } else {
      const h2_pal_result_t case_result =
          h2_h2loader_serial_e2e_status(&context);
      h2_h2loader_serial_e2e_record(
          out_result, H2_H2LOADER_SERIAL_E2E_CASE_STATUS, case_result);
      dependent_blocked = case_result != H2_PAL_OK;
    }
  }
  if ((config->suite_mask & H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND) != 0u) {
    if (dependent_blocked) {
      h2_h2loader_serial_e2e_skip(out_result,
                                  H2_H2LOADER_SERIAL_E2E_CASE_COMMAND);
    } else {
      h2_h2loader_serial_e2e_record(
          out_result, H2_H2LOADER_SERIAL_E2E_CASE_COMMAND,
          h2_h2loader_serial_e2e_command(&context));
    }
  }
  if ((config->suite_mask & H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL) != 0u) {
    if (dependent_blocked) {
      h2_h2loader_serial_e2e_skip(out_result,
                                  H2_H2LOADER_SERIAL_E2E_CASE_INSTALL);
    } else {
      h2_h2loader_serial_e2e_record(
          out_result, H2_H2LOADER_SERIAL_E2E_CASE_INSTALL,
          h2_h2loader_serial_e2e_install(&context));
    }
  }
  (void)h2_h2loader_serial_e2e_disconnect(&context);
  uint64_t finished = started;
  (void)h2_pal_time_get_monotonic_ms(runtime->time, &finished);
  out_result->elapsed_ms = finished >= started ? finished - started : 0u;
  if (out_result->result == H2_PAL_OK &&
      out_result->cleanup_result != H2_PAL_OK) {
    out_result->result = out_result->cleanup_result;
  }
  out_result->complete = 1;
  return out_result->result;
}
