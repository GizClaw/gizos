#include "h2_loader_app_client.h"
#include "h2_loader_task_names.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_LOADER_APP_CLIENT_POLL_MS 50u
#define H2_LOADER_APP_CLIENT_MIN_STACK_SIZE 8192u

typedef struct h2_loader_app_client_return_console {
    h2_loader_app_client_t *client;
    void *read_user;
    int (*read_byte)(void *user, uint32_t timeout_ms);
    void *write_user;
    h2_loader_app_client_write_fn write;
    atomic_bool stop_requested;
    int session_reset;
    int session_closed;
    h2_loader_command_t command;
} h2_loader_app_client_return_console_t;

typedef struct h2_loader_app_client_output {
    void *user;
    h2_loader_app_client_write_fn write;
} h2_loader_app_client_output_t;

static h2_pal_result_t console_read(
    void *user, void *buffer, size_t len, size_t *out_read,
    uint32_t timeout_ms) {
    h2_loader_app_client_return_console_t *console = user;
    int value;
    if (console == NULL || buffer == NULL || len == 0u || out_read == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_read = 0u;
    value = console->read_byte(console->read_user, timeout_ms);
    if (value == H2_LOADER_APP_CLIENT_SESSION_RESET) {
        console->session_reset = 1;
        return H2_PAL_ERR_TIMEOUT;
    }
    if (value == H2_LOADER_APP_CLIENT_SESSION_CLOSED) {
        console->session_closed = 1;
        return H2_PAL_ERR_CLOSED;
    }
    if (value == EOF) return H2_PAL_ERR_TIMEOUT;
    ((uint8_t *)buffer)[0] = (uint8_t)value;
    *out_read = 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t console_write(
    void *user, const void *buffer, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
    h2_loader_app_client_return_console_t *console = user;
    (void)timeout_ms;
    if (console == NULL || (buffer == NULL && len != 0u) || out_written == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    int rc = console->write(console->write_user, buffer, len);
    if (rc == H2_PAL_OK) *out_written = len;
    return (h2_pal_result_t)rc;
}

static h2_pal_result_t console_flush(void *user) {
    (void)user;
    return H2_PAL_OK;
}

static const h2_command_io_vtable_t s_console_io_vtable = {
    .read = console_read,
    .write = console_write,
    .flush = console_flush,
};

static h2_pal_result_t output_read(
    void *user, void *buffer, size_t len, size_t *out_read,
    uint32_t timeout_ms) {
    (void)user;
    (void)buffer;
    (void)len;
    (void)timeout_ms;
    if (out_read == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_read = 0u;
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t output_write(
    void *user, const void *buffer, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
    h2_loader_app_client_output_t *output = user;
    (void)timeout_ms;
    if (output == NULL || output->write == NULL || out_written == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    int rc = output->write(output->user, buffer, len);
    if (rc == H2_PAL_OK) *out_written = len;
    return (h2_pal_result_t)rc;
}

static const h2_command_io_vtable_t s_output_io_vtable = {
    .read = output_read,
    .write = output_write,
    .flush = console_flush,
};

static int stdout_write(void *user, const char *data, size_t len) {
    (void)user;
    if (data == NULL) return H2_PAL_ERR_INVALID_ARG;
    return fwrite(data, 1u, len, stdout) == len && fflush(stdout) == 0
        ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_loader_command_config_t command_config(
    h2_loader_app_client_t *client, h2_command_io_api_t io) {
    return (h2_loader_command_config_t){
        .loader = &client->loader,
        .fs = client->config.fs,
        .http = client->config.http,
        .wifi = client->config.wifi,
        .wifi_settings = client->config.wifi_settings,
        .disk = client->config.disk,
        .digest = client->config.digest,
        .memory_stats = client->config.memory_stats,
        .clock_user = client->config.clock_user,
        .now_ms = client->config.now_ms,
        .sleep_ms = client->config.sleep_ms,
        .io = io,
        .coredump_partition_id = client->config.coredump_partition_id,
        .operation_sync = client->config.operation_sync,
        .operation_mutex = client->config.operation_mutex,
        .wifi_operation_sync = client->config.wifi_operation_sync,
        .wifi_operation_mutex = client->config.wifi_operation_mutex,
        .defer_app_install = 1,
    };
}

int h2_loader_app_client_init(
    h2_loader_app_client_t *client,
    const h2_loader_app_client_config_t *config) {
    h2_loader_config_t loader_config;
    if (client == NULL || config == NULL || config->pref == NULL ||
        config->power == NULL || config->allocator == NULL || config->fs == NULL ||
        config->http == NULL || config->wifi == NULL || config->disk == NULL ||
        config->digest.start == NULL || config->digest.update == NULL ||
        config->digest.finish == NULL || config->now_ms == NULL ||
        config->sleep_ms == NULL || config->hardware_capabilities == 0u ||
        config->h2loader_partition_id == 0u || config->app_partition_id == 0u ||
        (config->hardware_capabilities & ~H2_LOADER_CAPABILITIES_ALL) != 0u ||
        ((config->operation_sync == NULL) != (config->operation_mutex == NULL)) ||
        ((config->wifi_operation_sync == NULL) !=
         (config->wifi_operation_mutex == NULL))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(client, 0, sizeof(*client));
    client->config = *config;
    memset(&loader_config, 0, sizeof(loader_config));
    loader_config.package.fs = config->fs;
    loader_config.package.disk = config->disk;
    loader_config.package.allocator = config->allocator;
    loader_config.package.digest = config->digest;
    loader_config.pref = config->pref;
    loader_config.power = config->power;
    loader_config.board = config->board;
    loader_config.target = config->target;
    loader_config.chip = config->chip;
    loader_config.device_uid = config->device_uid;
    loader_config.h2loader_partition_id = config->h2loader_partition_id;
    loader_config.app_partition_id = config->app_partition_id;
    loader_config.hardware_capabilities = config->hardware_capabilities;
    loader_config.active_identity = config->active_identity;
    return h2_loader_init(&client->loader, &loader_config);
}

int h2_loader_app_client_coredump(
    h2_loader_app_client_t *client, const char *subcommand,
    void *write_user, h2_loader_app_client_write_fn write) {
    h2_loader_app_client_output_t output = {
        .user = write_user,
        .write = write != NULL ? write : stdout_write,
    };
    h2_loader_command_t command;
    h2_loader_command_config_t config;
    const char *argv[3] = {"h2loader", "coredump", subcommand};
    size_t argc = subcommand != NULL && subcommand[0] != '\0' ? 3u : 2u;
    if (client == NULL) return H2_PAL_ERR_INVALID_ARG;
    config = command_config(client, (h2_command_io_api_t){
        .user = &output,
        .vtable = &s_output_io_vtable,
    });
    int rc = h2_loader_command_init(&command, &config);
    return rc == H2_PAL_OK
        ? h2_loader_command_execute(&command, argc, argv) : rc;
}

static void return_console_task(void *ctx) {
    h2_loader_app_client_return_console_t *console = ctx;
    h2_loader_command_config_t config;
    if (console == NULL || console->client == NULL || console->read_byte == NULL) {
        return;
    }
    config = command_config(console->client, (h2_command_io_api_t){
        .user = console,
        .vtable = &s_console_io_vtable,
    });
    if (h2_loader_command_init(&console->command, &config) != H2_PAL_OK) return;
    static const char ready[] = "H2_LOADER_APP_COMMAND_READY status=ready\n";
    (void)console->write(console->write_user, ready, sizeof(ready) - 1u);
    while (!atomic_load_explicit(&console->stop_requested, memory_order_acquire)) {
        int rc = h2_loader_command_poll(
            &console->command, H2_LOADER_APP_CLIENT_POLL_MS);
        if (console->session_closed || rc == H2_PAL_ERR_CLOSED) break;
        if (console->session_reset) {
            console->session_reset = 0;
            (void)h2_loader_command_init(&console->command, &config);
        }
    }
}

int h2_loader_app_client_start_return_console(
    const h2_loader_app_client_return_console_config_t *config) {
    h2_loader_app_client_return_console_t *console;
    h2_pal_task_t *task = NULL;
    h2_pal_task_options_t options;
    if (config == NULL || config->client == NULL || config->task == NULL ||
        config->read_byte == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (config->client->return_console_task != NULL ||
        config->client->return_console_private != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    console = h2_pal_mem_alloc(config->client->config.allocator, sizeof(*console));
    if (console == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(console, 0, sizeof(*console));
    console->client = config->client;
    console->read_user = config->read_user;
    console->read_byte = config->read_byte;
    console->write_user = config->write_user;
    console->write = config->write != NULL ? config->write : stdout_write;
    atomic_init(&console->stop_requested, false);
    options.name = config->task_name != NULL
        ? config->task_name : h2_loader_return_task_name;
    options.min_stack_size = config->stack_size > H2_LOADER_APP_CLIENT_MIN_STACK_SIZE
        ? config->stack_size : H2_LOADER_APP_CLIENT_MIN_STACK_SIZE;
    int rc = h2_pal_task_start(
        config->task, &options, return_console_task, console, &task);
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(config->client->config.allocator, console);
        return rc;
    }
    config->client->return_console_task_api = config->task;
    config->client->return_console_task = task;
    config->client->return_console_private = console;
    return H2_PAL_OK;
}

static int finish_return_console(h2_loader_app_client_t *client, int stop) {
    if (client == NULL || client->return_console_task == NULL ||
        client->return_console_task_api == NULL ||
        client->return_console_private == NULL) return H2_PAL_ERR_INVALID_STATE;
    h2_loader_app_client_return_console_t *console = client->return_console_private;
    if (stop) atomic_store_explicit(
        &console->stop_requested, true, memory_order_release);
    int rc = h2_pal_task_join(
        client->return_console_task_api, client->return_console_task);
    if (rc != H2_PAL_OK) return rc;
    h2_pal_mem_free(client->config.allocator, console);
    client->return_console_task_api = NULL;
    client->return_console_task = NULL;
    client->return_console_private = NULL;
    return H2_PAL_OK;
}

int h2_loader_app_client_join_return_console(h2_loader_app_client_t *client) {
    return finish_return_console(client, 0);
}

int h2_loader_app_client_stop_return_console(h2_loader_app_client_t *client) {
    return finish_return_console(client, 1);
}
