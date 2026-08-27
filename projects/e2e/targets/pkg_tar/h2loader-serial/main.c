#include "h2_h2loader_serial_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_H2LOADER_MAX_RESOURCES 32u
#define H2_WEB_H2LOADER_TASK_STACK_SIZE (128u * 1024u)

typedef struct h2_web_h2loader_resource {
  char name[H2_H2LOADER_HOST_RESOURCE_NAME_MAX_LEN];
  uint8_t *bytes;
  size_t len;
} h2_web_h2loader_resource_t;

typedef struct h2_web_h2loader_app {
  h2_web_platform_t *platform;
  h2_runtime_t *runtime;
  h2_pal_task_t *task;
  h2_h2loader_serial_e2e_result_t result;
  h2_pal_result_t run_result;
  h2_pal_result_t shutdown_result;
  char port_id[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
  int started;
  int complete;
  int cancelled;
  int shutting_down;
  int finalized;
  uint32_t suite;
  h2_h2loader_host_command_t command;
  char expected_board[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
  char expected_target[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
  char asset_sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
  uint8_t *catalog_json;
  size_t catalog_json_len;
  h2_web_h2loader_resource_t resources[H2_WEB_H2LOADER_MAX_RESOURCES];
  size_t resource_count;
} h2_web_h2loader_app_t;

static h2_web_h2loader_app_t app;

static void h2_web_h2loader_print_ledger(
    const h2_h2loader_serial_e2e_result_t *result) {
  for (size_t index = 0u; index < result->case_count; ++index) {
    printf("H2_WEB_H2LOADER_CASE id=%d rc=%d\n",
           result->cases[index].case_id, result->cases[index].result);
  }
  printf("H2_WEB_H2LOADER_INITIAL board=%s target=%s role=%u "
         "name=%s version=%s checksum=%s states=0x%016llx\n",
         result->initial_status.board, result->initial_status.target,
         (unsigned int)h2_h2loader_host_status_active_role(
             &result->initial_status),
         result->initial_status.active_name,
         result->initial_status.active_version,
         result->initial_status.active_checksum,
         (unsigned long long)result->initial_status.states);
  printf("H2_WEB_H2LOADER_FINAL board=%s target=%s role=%u "
         "name=%s version=%s checksum=%s states=0x%016llx\n",
         result->final_status.board, result->final_status.target,
         (unsigned int)h2_h2loader_host_status_active_role(
             &result->final_status),
         result->final_status.active_name,
         result->final_status.active_version,
         result->final_status.active_checksum,
         (unsigned long long)result->final_status.states);
  printf("H2_WEB_H2LOADER_METRICS command_bytes=%zu command_transport=%d "
         "command_terminal=%d command_truncated=%u command_lifecycle=%u "
         "acknowledged=%llu "
         "total=%llu elapsed_ms=%llu skipped=%zu cleanup=%d complete=%d\n",
         result->command_output_bytes, (int)result->command_transport_result,
         (int)result->command_terminal,
         (unsigned int)result->command_output_truncated,
         (unsigned int)result->command_lifecycle_transition,
         (unsigned long long)result->acknowledged_bytes,
         (unsigned long long)result->total_bytes,
         (unsigned long long)result->elapsed_ms, result->skipped,
         result->cleanup_result, result->complete);
}

static void h2_web_h2loader_release_resources(void) {
  free(app.catalog_json);
  app.catalog_json = NULL;
  app.catalog_json_len = 0u;
  for (size_t index = 0u; index < app.resource_count; ++index) {
    free(app.resources[index].bytes);
    app.resources[index].bytes = NULL;
  }
  app.resource_count = 0u;
}

EM_JS(void, h2_web_h2loader_present,
      (int result, size_t passed, size_t failed, size_t ports), {
  const element = globalThis.document && document.getElementById('result');
  if (element) {
    element.textContent = result === 0
      ? `PASS cases=${passed} ports=${ports}`
      : `FAIL rc=${result} passed=${passed} failed=${failed}`;
    element.dataset.terminal = result === 0 ? 'pass' : 'fail';
  }
});

EM_JS(void, h2_web_h2loader_progress, (uint64_t acknowledged, uint64_t total), {
  const element = globalThis.document && document.getElementById('progress');
  if (element) element.textContent = `${acknowledged}/${total}`;
});

static int h2_web_h2loader_cancelled(void *user) {
  return ((h2_web_h2loader_app_t *)user)->cancelled;
}

static h2_pal_result_t h2_web_h2loader_read_resource(
    void *user, const char *name, uint64_t offset, uint8_t *out,
    size_t out_size, size_t *out_read) {
  h2_web_h2loader_app_t *state = user;
  if (name == NULL || out_read == NULL || (out == NULL && out_size != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_read = 0u;
  for (size_t index = 0u; index < state->resource_count; ++index) {
    h2_web_h2loader_resource_t *resource = &state->resources[index];
    if (strcmp(name, resource->name) != 0) continue;
    if (offset >= resource->len) return H2_PAL_OK;
    size_t count = resource->len - (size_t)offset;
    if (count > out_size) count = out_size;
    if (count != 0u) {
      memcpy(out, resource->bytes + (size_t)offset, count);
    }
    *out_read = count;
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static void h2_web_h2loader_run(void *user) {
  h2_web_h2loader_app_t *state = user;
  const h2_h2loader_serial_e2e_config_t config = {
      .suite_mask = state->suite,
      .serial = h2_web_platform_serial_host_api(state->platform),
      .port_id = state->port_id,
      .expected_board = state->expected_board,
      .expected_target = state->expected_target,
      .command = state->command,
      .catalog_json = state->catalog_json,
      .catalog_json_len = state->catalog_json_len,
      .asset_sha256 = state->asset_sha256,
      .read_resource = h2_web_h2loader_read_resource,
      .resource_user = state,
      .is_cancelled = h2_web_h2loader_cancelled,
      .cancel_user = state,
  };
  state->run_result =
      h2_h2loader_serial_e2e_run(state->runtime, &config, &state->result);
  state->complete = 1;
}

EMSCRIPTEN_KEEPALIVE int h2_web_h2loader_request_port(void) {
  if (app.shutting_down || app.platform == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_web_platform_serial_request_port(app.platform);
}

EMSCRIPTEN_KEEPALIVE int h2_web_h2loader_authorization_status(void) {
  if (app.shutting_down || app.platform == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return h2_web_platform_serial_authorization(
      app.platform, app.port_id, sizeof(app.port_id));
}

static int h2_web_h2loader_copy_text(char *out, size_t out_size,
                                     const char *value) {
  if (value == NULL || strlen(value) >= out_size) return 0;
  strcpy(out, value);
  return 1;
}

static int h2_web_h2loader_sha256_valid(const char *sha256) {
  if (sha256 == NULL ||
      strlen(sha256) != H2_H2LOADER_HOST_SHA256_HEX_LEN) {
    return 0;
  }
  for (size_t index = 0u; index < H2_H2LOADER_HOST_SHA256_HEX_LEN; ++index) {
    if (!((sha256[index] >= '0' && sha256[index] <= '9') ||
          (sha256[index] >= 'a' && sha256[index] <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

EMSCRIPTEN_KEEPALIVE int h2_web_h2loader_configure(
    uint32_t suite, int command, const char *board, const char *target,
    const char *sha256) {
  const uint32_t known_suites = H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT |
                                H2_H2LOADER_SERIAL_E2E_SUITE_STATUS |
                                H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND |
                                H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL;
  const int suite_valid = suite != 0u && (suite & ~known_suites) == 0u;
  const int command_valid = command >= H2_H2LOADER_HOST_COMMAND_HELP &&
                            command <= H2_H2LOADER_HOST_COMMAND_MEMORY;
  if (app.started || app.shutting_down || !suite_valid ||
      !h2_web_h2loader_copy_text(app.expected_board,
                                  sizeof(app.expected_board), board) ||
      !h2_web_h2loader_copy_text(app.expected_target,
                                  sizeof(app.expected_target), target) ||
      !h2_web_h2loader_copy_text(app.asset_sha256,
                                  sizeof(app.asset_sha256), sha256)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (((suite & H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND) != 0u &&
       !command_valid) ||
      ((suite & H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL) != 0u &&
       (app.expected_board[0] == '\0' || app.expected_target[0] == '\0' ||
        !h2_web_h2loader_sha256_valid(app.asset_sha256)))) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  app.suite = suite;
  app.command = (h2_h2loader_host_command_t)command;
  return H2_PAL_OK;
}

EMSCRIPTEN_KEEPALIVE uintptr_t h2_web_h2loader_allocate_catalog(size_t len) {
  if (app.started || app.shutting_down || len == 0u) return 0u;
  uint8_t *copy = malloc(len);
  if (copy == NULL) return 0u;
  free(app.catalog_json);
  app.catalog_json = copy;
  app.catalog_json_len = len;
  return (uintptr_t)copy;
}

static int h2_web_h2loader_resource_name_valid(const char *name) {
  if (name == NULL || name[0] == '\0' || name[0] == '/' ||
      strchr(name, '\\') != NULL) {
    return 0;
  }
  const char *segment = name;
  for (const char *cursor = name;; ++cursor) {
    if (*cursor != '/' && *cursor != '\0') continue;
    const size_t length = (size_t)(cursor - segment);
    if (length == 0u || (length == 1u && segment[0] == '.') ||
        (length == 2u && segment[0] == '.' && segment[1] == '.')) {
      return 0;
    }
    if (*cursor == '\0') return 1;
    segment = cursor + 1;
  }
}

EMSCRIPTEN_KEEPALIVE uintptr_t h2_web_h2loader_allocate_resource(
    const char *name, size_t len) {
  if (app.started || app.shutting_down ||
      !h2_web_h2loader_resource_name_valid(name) || len == 0u ||
      app.resource_count >= H2_WEB_H2LOADER_MAX_RESOURCES ||
      strlen(name) >= sizeof(app.resources[0].name)) {
    return 0u;
  }
  for (size_t index = 0u; index < app.resource_count; ++index) {
    if (strcmp(name, app.resources[index].name) == 0) return 0u;
  }
  uint8_t *copy = malloc(len);
  if (copy == NULL) return 0u;
  h2_web_h2loader_resource_t *resource =
      &app.resources[app.resource_count++];
  strcpy(resource->name, name);
  resource->bytes = copy;
  resource->len = len;
  return (uintptr_t)copy;
}

EMSCRIPTEN_KEEPALIVE int h2_web_h2loader_reset_resources(void) {
  if (app.shutting_down) return H2_PAL_ERR_INVALID_STATE;
  if (app.started) return H2_PAL_ERR_BUSY;
  h2_web_h2loader_release_resources();
  return H2_PAL_OK;
}

EMSCRIPTEN_KEEPALIVE int h2_web_h2loader_write_allocation(
    uintptr_t destination, size_t total_len, size_t offset,
    const uint8_t *bytes, size_t len) {
  if (app.started || app.shutting_down || destination == 0u || bytes == NULL ||
      len == 0u ||
      offset > total_len || len > total_len - offset) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint8_t *allocation = (uint8_t *)destination;
  int found = allocation == app.catalog_json && total_len == app.catalog_json_len;
  for (size_t index = 0u; !found && index < app.resource_count; ++index) {
    found = allocation == app.resources[index].bytes &&
            total_len == app.resources[index].len;
  }
  if (!found) return H2_PAL_ERR_NOT_FOUND;
  memcpy(allocation + offset, bytes, len);
  return H2_PAL_OK;
}

EMSCRIPTEN_KEEPALIVE void h2_web_h2loader_cancel(void) {
  app.cancelled = 1;
}

EMSCRIPTEN_KEEPALIVE int h2_web_h2loader_start(void) {
  if (app.started || app.shutting_down || app.platform == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_result_t result = H2_PAL_OK;
  if (app.suite != H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT) {
    result = h2_web_platform_serial_authorization(
        app.platform, app.port_id, sizeof(app.port_id));
    if (result != H2_PAL_OK) return result;
  } else {
    app.port_id[0] = '\0';
  }
  const h2_pal_task_options_t task_options = {
      .name = "h2loader-serial-e2e",
      .min_stack_size = H2_WEB_H2LOADER_TASK_STACK_SIZE,
  };
  result = h2_pal_task_start(h2_web_platform_task_api(app.platform),
                             &task_options, h2_web_h2loader_run, &app,
                             &app.task);
  if (result == H2_PAL_OK) {
    app.started = 1;
#if defined(H2_WEB_H2LOADER_INTERACTIVE)
    h2_web_platform_schedule(app.platform);
#endif
  }
  return result;
}

static h2_pal_result_t h2_web_h2loader_finalize(int present_result) {
  if (app.finalized) return H2_PAL_OK;
  h2_pal_result_t result = app.run_result;
  if (app.task != NULL) {
    const h2_pal_result_t join_result = h2_pal_task_join(
        h2_web_platform_task_api(app.platform), app.task);
    if (join_result != H2_PAL_OK) return join_result;
    app.task = NULL;
    h2_web_h2loader_print_ledger(&app.result);
    if (present_result) {
      h2_web_h2loader_present(app.run_result, app.result.passed,
                              app.result.failed,
                              app.result.enumerated_ports);
    }
  }
  if (app.runtime != NULL) {
    h2_runtime_deinit(app.runtime);
    app.runtime = NULL;
  }
  if (app.platform != NULL) {
    h2_web_platform_destroy(app.platform);
    app.platform = NULL;
  }
  h2_web_h2loader_release_resources();
  app.finalized = 1;
  return result;
}

static h2_pal_result_t h2_web_h2loader_advance(int present_result) {
  if (app.platform == NULL) return H2_PAL_ERR_INVALID_STATE;
  h2_pal_result_t result = h2_web_platform_pump(app.platform, 32u, NULL);
  h2_web_h2loader_progress(app.result.acknowledged_bytes,
                            app.result.total_bytes);
  if (result != H2_PAL_OK) return result;
  if (!app.complete) {
    if (!app.shutting_down || app.task == NULL) {
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    const h2_pal_result_t join_result = h2_pal_task_join(
        h2_web_platform_task_api(app.platform), app.task);
    if (join_result == H2_PAL_ERR_BUSY) return H2_PAL_ERR_WOULD_BLOCK;
    if (join_result != H2_PAL_OK && join_result != H2_PAL_EXIT) {
      return join_result;
    }
    app.task = NULL;
  }
  return h2_web_h2loader_finalize(present_result);
}

EMSCRIPTEN_KEEPALIVE int h2_web_h2loader_tick(void) {
  if (app.shutting_down) return H2_PAL_ERR_INVALID_STATE;
  h2_web_h2loader_progress(app.result.acknowledged_bytes,
                            app.result.total_bytes);
  const h2_pal_result_t result = app.complete
                                     ? h2_web_h2loader_finalize(1)
                                     : H2_PAL_ERR_WOULD_BLOCK;
  if (result != H2_PAL_ERR_WOULD_BLOCK) {
    printf("H2_WEB_H2LOADER_TICK rc=%d complete=%d task=%s finalized=%d\n",
           result, app.complete, app.task == NULL ? "none" : "owned",
           app.finalized);
  }
  return result;
}

EMSCRIPTEN_KEEPALIVE int h2_web_h2loader_shutdown_step(void) {
  if (!app.shutting_down) {
    app.shutting_down = 1;
    app.cancelled = 1;
    if (app.platform != NULL) {
      const h2_pal_result_t serial_result =
          h2_web_platform_serial_shutdown(app.platform);
      if (serial_result != H2_PAL_OK &&
          serial_result != H2_PAL_ERR_UNSUPPORTED) {
        return serial_result;
      }
      app.shutdown_result = serial_result;
      if (app.task != NULL) {
        const h2_pal_result_t task_result =
            h2_web_platform_task_cancel(app.platform, app.task);
        if (task_result != H2_PAL_OK) return task_result;
      }
    }
  }
  if (app.finalized) return H2_PAL_OK;
  if (!app.started) return h2_web_h2loader_finalize(0);
  const h2_pal_result_t result = h2_web_h2loader_advance(0);
  if (result == H2_PAL_ERR_WOULD_BLOCK) return result;
  if (result != H2_PAL_OK && result != H2_PAL_EXIT) return result;
  return app.shutdown_result != H2_PAL_OK ? app.shutdown_result : result;
}

int main(void) {
  const h2_web_platform_config_t platform_config = {1, 1};
  app.platform = h2_web_platform_create(&platform_config);
  if (app.platform == NULL) return 1;
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(app.platform),
      h2_web_platform_queue_api(app.platform),
      h2_web_platform_display_api(app.platform));
  config.log = h2_web_platform_log_api();
  config.timer = h2_web_platform_timer_api(app.platform);
  config.task = h2_web_platform_task_api(app.platform);
  config.sync = h2_web_platform_sync_api(app.platform);
  config.webrtc = h2_web_platform_webrtc_api(app.platform);
  config.webrtc_media_track = h2_web_platform_webrtc_audio_track(app.platform);
  h2_pal_result_t result = h2_runtime_init(&config, &app.runtime);
  if (result != H2_PAL_OK) {
    h2_web_platform_destroy(app.platform);
    app.platform = NULL;
    return 1;
  }
#if defined(H2_WEB_H2LOADER_FAKE_AUTO)
  app.suite = H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT;
  app.command = H2_H2LOADER_HOST_COMMAND_STATUS;
  result = (h2_pal_result_t)h2_web_h2loader_start();
  for (int turn = 0; result == H2_PAL_OK && turn < 128; ++turn) {
    result = h2_web_h2loader_advance(1);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
      result = H2_PAL_OK;
      emscripten_sleep(1u);
    } else {
      break;
    }
  }
  printf("H2_WEB_H2LOADER_SERIAL_E2E result=%s rc=%d passed=%zu failed=%zu skipped=%zu ports=%zu\n",
         result == H2_PAL_OK ? "PASS" : "FAIL", result, app.result.passed,
         app.result.failed, app.result.skipped, app.result.enumerated_ports);
  if (app.runtime != NULL) h2_runtime_deinit(app.runtime);
  if (app.platform != NULL) h2_web_platform_destroy(app.platform);
  h2_web_h2loader_release_resources();
  return result == H2_PAL_OK ? 0 : 1;
#else
  puts("H2_WEB_H2LOADER_SERIAL_READY");
  return 0;
#endif
}
