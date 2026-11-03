#include "h2loader_bleikcp_internal.h"
#include "h2loader_app_task_names.h"

#include <string.h>

typedef struct h2loader_ble_service {
  const h2_pal_task_api_t* task_api;
  const h2_pal_time_api_t* time_api;
  const h2_pal_mem_api_t* allocator;
  h2_loader_command_config_t command_config;
  h2_loader_ble_service_t* service;
  bool inline_commands;
} h2loader_ble_service_t;

typedef struct h2loader_ble_session {
  h2_loader_command_config_t command_config;
  h2_loader_command_t command;
  h2_bleikcp_t* stream;
  int result;
} h2loader_ble_session_t;

static void command_task(void* ctx) {
  h2loader_ble_session_t* session = ctx;
  session->command_config.io = h2_loader_ble_command_io(session->stream);
  session->command_config.transport = H2_LOADER_COMMAND_TRANSPORT_BLE;
  session->result =
      h2_loader_command_init(&session->command, &session->command_config);
  while (session->result == H2_PAL_OK) {
    int rc = h2_loader_command_poll(&session->command, 50u);
    if (rc == H2_PAL_OK || rc == H2_PAL_ERR_TIMEOUT ||
        rc == H2_PAL_ERR_WOULD_BLOCK) {
      continue;
    }
    if (rc == H2_PAL_ERR_CLOSED) {
      session->result = H2_PAL_OK;
      break;
    }
    session->result = rc;
  }
}

static int handle_session(void* user, h2_bleikcp_t* stream,
                          uint16_t conn_handle) {
  const h2loader_ble_service_t* base = user;
  h2loader_ble_session_t session;
  h2_pal_task_t* task = NULL;
  const h2_pal_task_options_t options = {
      /* Separate from the persistent UART command task: name-addressed
       * backends must not delete that task when a BLE session is joined. */
      .name = h2loader_ble_command_task_name,
      .min_stack_size = 49152u,
  };
  (void)conn_handle;
  if (base == NULL || stream == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  session = (h2loader_ble_session_t){
      .command_config = base->command_config,
      .stream = stream,
      .result = H2_PAL_OK,
  };
  if (base->inline_commands) {
    command_task(&session);
    return session.result;
  }
  int rc = h2_pal_task_start(base->task_api, &options, command_task, &session,
                             &task);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  /* A failed join does not release the task or its borrowed stack context. */
  while (h2_pal_task_join(base->task_api, task) != H2_PAL_OK) {
    (void)h2_pal_time_sleep_ms(base->time_api, 10u);
  }
  return session.result;
}

static int close_command_service(void* user, void* service) {
  h2loader_ble_service_t* owned = service;
  (void)user;
  if (owned == NULL) {
    return H2_PAL_OK;
  }
  if (owned->service != NULL) {
    int rc = h2_loader_ble_service_close(owned->service);
    if (rc != H2_PAL_OK) {
      return rc;
    }
    owned->service = NULL;
  }
  int rc = h2_bleikcp_global_shutdown();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  h2_pal_mem_free(owned->allocator, owned);
  return H2_PAL_OK;
}

static bool valid_config(const h2loader_ble_command_service_config_t* config) {
  return config == NULL || !config->inline_commands ||
         config->server_task_stack_size >= 32768u;
}

static int open_command_service(
    void* user, h2_runtime_t* runtime,
    const h2_loader_command_config_t* command_config, const char* board,
    uint32_t capabilities, void** out_service) {
  const h2loader_ble_command_service_config_t* config = user;
  if (out_service == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_service = NULL;
  if (runtime == NULL || runtime->mem == NULL || command_config == NULL ||
      board == NULL || !valid_config(config)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2loader_ble_service_t* owned =
      h2_pal_mem_alloc(runtime->mem, sizeof(*owned));
  if (owned == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  *owned = (h2loader_ble_service_t){
      .task_api = runtime->task,
      .time_api = runtime->time,
      .allocator = runtime->mem,
      .command_config = *command_config,
      .inline_commands = config != NULL && config->inline_commands,
  };
  int rc = h2_bleikcp_global_init();
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(runtime->mem, owned);
    return rc;
  }
  const h2_loader_ble_service_config_t service_config = {
      .log = runtime->log,
      .api =
          {
              .ble = runtime->ble_host,
              .task = runtime->task,
              .time = runtime->time,
              .sync = runtime->sync,
              .system_event = runtime->system_event,
              .allocator = runtime->mem,
          },
      .board = board,
      .local_name = runtime->ble_local_name,
      .capabilities = capabilities,
      .advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY,
      .handler = handle_session,
      .handler_user = owned,
      .server_task_stack_size = config != NULL ? config->server_task_stack_size
                                                : 0u,
      .stream_buffer_size = config != NULL ? config->stream_buffer_size : 0u,
      .stream_window = config != NULL ? config->stream_window : 0u,
      .input_frame_capacity = config != NULL ? config->input_frame_capacity : 0u,
  };
  rc = h2_loader_ble_service_open(&service_config, &owned->service);
  if (rc == H2_PAL_OK || close_command_service(NULL, owned) != H2_PAL_OK) {
    /* A partial-open cleanup failure retains the same close/retry ownership
     * as an ordinary service close failure. Never drop its borrowed context. */
    *out_service = owned;
  }
  return rc;
}

int h2loader_bleikcp_command_service_with_config(
    const h2loader_ble_command_service_config_t* config,
    h2loader_app_command_service_api_t* out_api) {
  if (out_api == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_api, 0, sizeof(*out_api));
  if (!valid_config(config)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_api = (h2loader_app_command_service_api_t){
      .user = (void*)config,
      .open = open_command_service,
      .close = close_command_service,
  };
  return H2_PAL_OK;
}

const h2loader_app_command_service_api_t* h2loader_bleikcp_command_service(
    void) {
  static const h2loader_app_command_service_api_t api = {
      .open = open_command_service,
      .close = close_command_service,
  };
  return &api;
}
