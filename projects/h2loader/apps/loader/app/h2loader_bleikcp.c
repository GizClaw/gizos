#include "h2loader_bleikcp_internal.h"

typedef struct h2loader_ble_session {
    const h2_pal_task_api_t *task_api;
    h2_loader_command_config_t command_config;
    h2_loader_command_t command;
    h2_bleikcp_t *stream;
    int result;
} h2loader_ble_session_t;

static void command_task(void *ctx) {
    h2loader_ble_session_t *session = ctx;
    session->command_config.io = h2_loader_ble_command_io(session->stream);
    session->result = h2_loader_command_init(
        &session->command, &session->command_config);
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

static int handle_session(
    void *user,
    h2_bleikcp_t *stream,
    uint16_t conn_handle) {
    const h2loader_ble_session_t *base = user;
    h2loader_ble_session_t session;
    h2_pal_task_t *task = NULL;
    const h2_pal_task_options_t options = {
        .name = "h2loader/appcmd",
        .min_stack_size = 49152u,
    };
    (void)conn_handle;
    if (base == NULL || stream == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    session = (h2loader_ble_session_t){
        .task_api = base->task_api,
        .command_config = base->command_config,
        .stream = stream,
        .result = H2_PAL_OK,
    };
    int rc = h2_pal_task_start(
        session.task_api, &options, command_task, &session, &task);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_pal_task_join(session.task_api, task);
    return rc == H2_PAL_OK ? session.result : rc;
}

static int open_command_service(
    void *user,
    h2_runtime_t *runtime,
    const h2_loader_command_config_t *command_config,
    const char *board,
    uint32_t capabilities,
    void **out_service) {
    (void)user;
    if (runtime == NULL || command_config == NULL || board == NULL ||
        out_service == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_service = NULL;
    static h2loader_ble_session_t base;
    base = (h2loader_ble_session_t){
        .task_api = runtime->task,
        .command_config = *command_config,
    };
    const h2_loader_ble_service_config_t service_config = {
        .api = {
            .ble = runtime->ble_host,
            .task = runtime->task,
            .time = runtime->time,
            .sync = runtime->sync,
            .system_event = runtime->system_event,
            .allocator = runtime->mem,
        },
        .board = board,
        .capabilities = capabilities,
        .handler = handle_session,
        .handler_user = &base,
    };
    h2_loader_ble_service_t *service = NULL;
    int rc = h2_loader_ble_service_open(&service_config, &service);
    if (rc == H2_PAL_OK) {
        *out_service = service;
    }
    return rc;
}

static int close_command_service(void *user, void *service) {
    (void)user;
    if (service == NULL) {
        return H2_PAL_OK;
    }
    return h2_loader_ble_service_close(service);
}

const h2loader_app_command_service_api_t *
h2loader_bleikcp_command_service(void) {
    static const h2loader_app_command_service_api_t api = {
        .open = open_command_service,
        .close = close_command_service,
    };
    return &api;
}
