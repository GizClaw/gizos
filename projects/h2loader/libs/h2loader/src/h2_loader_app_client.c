#include "h2_loader_app_client.h"

#include "h2_loader_boot.h"
#include "h2_loader_status.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_LOADER_APP_CLIENT_LINE_MAX 128u
#define H2_LOADER_APP_CLIENT_POLL_MS 50u
#define H2_LOADER_APP_CLIENT_MIN_STACK_SIZE 8192u

typedef struct h2_loader_app_client_return_console {
    h2_loader_app_client_t *client;
    void *read_user;
    int (*read_byte)(void *user, uint32_t timeout_ms);
    void *write_user;
    int (*write)(void *user, const char *data, size_t len);
    atomic_bool stop_requested;
    char status_line[H2_LOADER_STATUS_LINE_MAX];
} h2_loader_app_client_return_console_t;

static const char *default_if_empty(const char *value, const char *fallback) {
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static int split_args(char *line, const char **argv, int max_args) {
    int argc = 0;
    char *cursor = line;

    while (*cursor != '\0' && argc < max_args) {
        while (isspace((unsigned char)*cursor)) {
            *cursor++ = '\0';
        }
        if (*cursor == '\0') {
            break;
        }
        argv[argc++] = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            ++cursor;
        }
    }
    return argc;
}

static int is_return_command(int argc, const char *const *argv) {
    return argc == 2 &&
        strcmp(argv[0], "h2loader") == 0 &&
        strcmp(argv[1], "rollback") == 0;
}

static int is_restart_command(int argc, const char *const *argv) {
    return argc == 2 &&
        strcmp(argv[0], "h2loader") == 0 &&
        strcmp(argv[1], "restart") == 0;
}

static int is_status_command(int argc, const char *const *argv) {
    return argc == 2 &&
        strcmp(argv[0], "h2loader") == 0 &&
        (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "stats") == 0);
}

static int is_memory_command(int argc, const char *const *argv) {
    return argc == 2 &&
        strcmp(argv[0], "h2loader") == 0 &&
        strcmp(argv[1], "memory") == 0;
}

static int is_help_command(int argc, const char *const *argv) {
    return argc == 2 &&
        strcmp(argv[0], "h2loader") == 0 &&
        strcmp(argv[1], "help") == 0;
}

static int is_coredump_command(int argc, const char *const *argv) {
    return argc >= 2 && argc <= 3 &&
        strcmp(argv[0], "h2loader") == 0 &&
        strcmp(argv[1], "coredump") == 0;
}

static int stdout_write(void *user, const char *data, size_t len) {
    (void)user;
    if (data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len > 0u && fwrite(data, 1u, len, stdout) != len) {
        return H2_PAL_ERR_IO;
    }
    return fflush(stdout) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int write_output_line(
    void *write_user,
    h2_loader_app_client_write_fn write,
    const char *line) {
    int rc;

    if (write == NULL || line == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = write(write_user, line, strlen(line));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return write(write_user, "\n", 1u);
}

static int write_console_line(
    const h2_loader_app_client_return_console_t *console,
    const char *line) {
    if (console == NULL || line == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return write_output_line(console->write_user, console->write, line);
}

static int prepare_return_to_loader(h2_loader_app_client_t *client) {
    int rc;

    if (client == NULL || client->config.pref == NULL || client->config.power == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_mark_return_requested(client->config.pref);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (client->config.h2loader_partition_id == 0u) {
        return H2_PAL_OK;
    }
    return h2_pal_power_set_next_boot_partition(
        client->config.power,
        client->config.h2loader_partition_id);
}

static int write_status(h2_loader_app_client_return_console_t *console) {
    h2_loader_status_t status;
    char *line;
    int rc;

    if (console == NULL || console->client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    line = console->status_line;
    rc = h2_loader_read_pref_status(
        console->client->config.pref,
        console->client->config.allocator,
        &status);
    if (rc != H2_PAL_OK) {
        (void)snprintf(line, H2_LOADER_STATUS_LINE_MAX, "H2_LOADER_STATUS_ERROR code=%d", rc);
        return write_console_line(console, line);
    }
    rc = h2_loader_status_set_device(
        &status,
        console->client->config.board,
        console->client->config.target,
        console->client->config.chip);
    if (rc != H2_PAL_OK) {
        (void)snprintf(line, H2_LOADER_STATUS_LINE_MAX, "H2_LOADER_STATUS_ERROR code=%d", rc);
        return write_console_line(console, line);
    }
    rc = h2_loader_status_set_active(
        &status,
        "app",
        default_if_empty(console->client->config.active_name, "app"),
        console->client->config.active_version,
        console->client->config.active_checksum);
    status.capabilities = console->client->config.capabilities;
    if (rc == H2_PAL_OK) {
        rc = h2_loader_status_format(&status, line, H2_LOADER_STATUS_LINE_MAX);
    }
    if (rc != H2_PAL_OK) {
        (void)snprintf(line, H2_LOADER_STATUS_LINE_MAX, "H2_LOADER_STATUS_ERROR code=%d", rc);
    }
    return write_console_line(console, line);
}

static int write_memory(h2_loader_app_client_return_console_t *console) {
    h2_loader_memory_stats_t stats = {0};
    char line[512];
    int len;

    if (console == NULL || console->client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (console->client->config.memory_stats.read == NULL) {
        len = snprintf(
            line, sizeof(line),
            "H2_LOADER_MEMORY result=unsupported code=%d",
            H2_PAL_ERR_UNSUPPORTED);
        return len < 0 || (size_t)len >= sizeof(line)
            ? H2_PAL_ERR_NO_SPACE
            : write_console_line(console, line);
    }
    int rc = console->client->config.memory_stats.read(
        console->client->config.memory_stats.user, &stats);
    if (rc != H2_PAL_OK) {
        len = snprintf(line, sizeof(line),
            "H2_LOADER_MEMORY result=fail code=%d", rc);
    } else {
        len = snprintf(
            line, sizeof(line),
            "H2_LOADER_MEMORY result=OK internal_total=%zu internal_free=%zu "
            "internal_min_free=%zu internal_largest=%zu iram_total=%zu "
            "iram_free=%zu iram_min_free=%zu iram_largest=%zu "
            "psram_total=%zu psram_free=%zu psram_min_free=%zu "
            "psram_largest=%zu",
            stats.internal.total_bytes, stats.internal.free_bytes,
            stats.internal.minimum_free_bytes,
            stats.internal.largest_free_block_bytes, stats.iram.total_bytes,
            stats.iram.free_bytes, stats.iram.minimum_free_bytes,
            stats.iram.largest_free_block_bytes, stats.psram.total_bytes,
            stats.psram.free_bytes, stats.psram.minimum_free_bytes,
            stats.psram.largest_free_block_bytes);
    }
    if (len < 0 || (size_t)len >= sizeof(line)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    return write_console_line(console, line);
}

static int coredump_info(
    const h2_loader_app_client_t *client,
    const h2_pal_disk_partition_t *partition,
    uint64_t *out_stored_bytes,
    int *out_blank) {
    uint8_t head[16];
    uint32_t size;
    if (client == NULL || partition == NULL || out_stored_bytes == NULL ||
        out_blank == NULL || client->config.disk == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_stored_bytes = 0u;
    *out_blank = 1;
    int rc = h2_pal_disk_read(
        client->config.disk, partition->id, 0u, head, sizeof(head));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < sizeof(head); ++i) {
        if (head[i] != 0xffu && head[i] != 0x00u) {
            *out_blank = 0;
            break;
        }
    }
    if (*out_blank) {
        return H2_PAL_OK;
    }
    size = (uint32_t)head[0] |
        ((uint32_t)head[1] << 8u) |
        ((uint32_t)head[2] << 16u) |
        ((uint32_t)head[3] << 24u);
    if (size < sizeof(uint32_t) || size > partition->size) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_stored_bytes = size;
    return H2_PAL_OK;
}

static int write_coredump(
    h2_loader_app_client_t *client,
    const char *sub,
    void *write_user,
    h2_loader_app_client_write_fn write) {
    static const char hex[] = "0123456789abcdef";
    h2_pal_disk_partition_t partition;
    char line[384];
    if (client == NULL || write == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    sub = sub != NULL && sub[0] != '\0' ? sub : "status";
    int rc = h2_pal_disk_get_partition(
        client->config.disk,
        client->config.coredump_partition_id,
        &partition);
    if (rc != H2_PAL_OK) {
        (void)snprintf(line, sizeof(line),
            "H2_LOADER_COREDUMP command=%s result=unavailable code=%d", sub, rc);
        (void)write_output_line(write_user, write, line);
        return rc;
    }
    if (strcmp(sub, "status") == 0) {
        uint64_t stored_bytes = 0u;
        int blank = 1;
        rc = coredump_info(
            client, &partition, &stored_bytes, &blank);
        (void)snprintf(line, sizeof(line),
            "H2_LOADER_COREDUMP_STATUS result=%s code=%d partition=%s bytes=%llu stored_bytes=%llu blank=%d",
            rc == H2_PAL_OK ? "OK" : "fail", rc, partition.name,
            (unsigned long long)partition.size,
            (unsigned long long)stored_bytes, blank);
        (void)write_output_line(write_user, write, line);
        return rc;
    }
    if (strcmp(sub, "erase") == 0) {
        rc = h2_pal_disk_erase(
            client->config.disk, partition.id, 0u, partition.size);
        (void)snprintf(line, sizeof(line),
            "H2_LOADER_COREDUMP_ERASE result=%s code=%d",
            rc == H2_PAL_OK ? "OK" : "fail", rc);
        (void)write_output_line(write_user, write, line);
        return rc;
    }
    if (strcmp(sub, "dump") == 0) {
        uint8_t buffer[128];
        uint64_t stored_bytes = 0u;
        uint64_t offset = 0u;
        int blank = 1;
        rc = coredump_info(
            client, &partition, &stored_bytes, &blank);
        while (rc == H2_PAL_OK && offset < stored_bytes) {
            size_t take = stored_bytes - offset > sizeof(buffer)
                ? sizeof(buffer)
                : (size_t)(stored_bytes - offset);
            rc = h2_pal_disk_read(
                client->config.disk, partition.id, offset,
                buffer, take);
            if (rc != H2_PAL_OK) {
                break;
            }
            int prefix_len = snprintf(line, sizeof(line),
                "H2_LOADER_COREDUMP_DATA offset=%llu hex=",
                (unsigned long long)offset);
            if (prefix_len < 0 ||
                (size_t)prefix_len + take * 2u + 1u > sizeof(line)) {
                rc = H2_PAL_ERR_NO_SPACE;
                break;
            }
            for (size_t i = 0u; i < take; ++i) {
                line[(size_t)prefix_len + i * 2u] =
                    hex[(buffer[i] >> 4u) & 0x0fu];
                line[(size_t)prefix_len + i * 2u + 1u] =
                    hex[buffer[i] & 0x0fu];
            }
            line[(size_t)prefix_len + take * 2u] = '\0';
            rc = write_output_line(write_user, write, line);
            offset += take;
        }
        (void)snprintf(line, sizeof(line),
            "H2_LOADER_COREDUMP_DUMP result=%s code=%d bytes=%llu blank=%d",
            rc == H2_PAL_OK ? "OK" : "fail", rc,
            (unsigned long long)stored_bytes, blank);
        (void)write_output_line(write_user, write, line);
        return rc;
    }
    (void)snprintf(line, sizeof(line),
        "H2_LOADER_COREDUMP result=invalid_command command=%s", sub);
    (void)write_output_line(write_user, write, line);
    return H2_PAL_ERR_INVALID_ARG;
}

int h2_loader_app_client_coredump(
    h2_loader_app_client_t *client,
    const char *subcommand,
    void *write_user,
    h2_loader_app_client_write_fn write) {
    if (client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return write_coredump(
        client,
        subcommand,
        write_user,
        write != NULL ? write : stdout_write);
}

static void return_console_task(void *ctx) {
    h2_loader_app_client_return_console_t *console = (h2_loader_app_client_return_console_t *)ctx;
    char line[H2_LOADER_APP_CLIENT_LINE_MAX];
    size_t line_len = 0u;

    if (console == NULL || console->read_byte == NULL || console->client == NULL) {
        return;
    }
    (void)write_console_line(console, "H2_LOADER_APP_COMMAND_READY status=ready");
    while (!atomic_load_explicit(&console->stop_requested, memory_order_acquire)) {
        int ch = console->read_byte(console->read_user, H2_LOADER_APP_CLIENT_POLL_MS);
        if (ch == H2_LOADER_APP_CLIENT_SESSION_RESET) {
            line_len = 0u;
            continue;
        }
        if (ch == H2_LOADER_APP_CLIENT_SESSION_CLOSED) {
            break;
        }
        if (ch == EOF) {
            continue;
        }
        if (line_len + 1u >= sizeof(line)) {
            line_len = 0u;
            continue;
        }
        line[line_len++] = (char)ch;
        if (ch != '\n' && ch != '\r') {
            continue;
        }
        line[line_len] = '\0';
        line_len = 0u;

        const char *argv[4];
        int argc = split_args(line, argv, 4);
        bool recognized = is_status_command(argc, argv) ||
            is_memory_command(argc, argv) ||
            is_help_command(argc, argv) || is_coredump_command(argc, argv) ||
            is_return_command(argc, argv) || is_restart_command(argc, argv);
        int lock_rc = H2_PAL_OK;
        if (recognized && console->client->config.operation_mutex != NULL) {
            lock_rc = h2_pal_mutex_lock(
                console->client->config.operation_sync,
                console->client->config.operation_mutex);
        }
        if (lock_rc != H2_PAL_OK) {
            (void)write_console_line(
                console, "H2_LOADER_ERROR reason=operation_lock_failed");
            continue;
        }
        if (is_status_command(argc, argv)) {
            (void)write_status(console);
        } else if (is_memory_command(argc, argv)) {
            (void)write_memory(console);
        } else if (is_help_command(argc, argv)) {
            (void)write_console_line(console,
                "h2loader <help|status|stats|memory|restart|rollback|coredump>");
        } else if (is_coredump_command(argc, argv)) {
            (void)h2_loader_app_client_coredump(
                console->client,
                argc == 3 ? argv[2] : NULL,
                console->write_user,
                console->write);
        } else if (is_return_command(argc, argv)) {
            int return_rc = prepare_return_to_loader(console->client);
            if (return_rc != H2_PAL_OK) {
                char response[64];
                (void)snprintf(
                    response,
                    sizeof(response),
                    "H2_LOADER_ROLLBACK result=error code=%d",
                    return_rc);
                (void)write_console_line(console, response);
            } else {
                (void)write_console_line(
                    console, "H2_LOADER_ROLLBACK result=OK");
                (void)h2_pal_power_reboot(
                    console->client->config.power,
                    console->client->config.reboot_reason);
            }
        } else if (is_restart_command(argc, argv)) {
            if (write_console_line(
                    console, "H2_LOADER_RESTART result=OK") == H2_PAL_OK) {
                (void)h2_loader_app_client_restart(console->client);
            }
        }
        if (recognized && console->client->config.operation_mutex != NULL) {
            (void)h2_pal_mutex_unlock(
                console->client->config.operation_sync,
                console->client->config.operation_mutex);
        }
    }
}

int h2_loader_app_client_init(
    h2_loader_app_client_t *client,
    const h2_loader_app_client_config_t *config) {
    if (client == NULL || config == NULL || config->pref == NULL ||
        config->power == NULL || config->allocator == NULL ||
        ((config->operation_sync == NULL) !=
         (config->operation_mutex == NULL))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(client, 0, sizeof(*client));
    client->config = *config;
    if (client->config.capabilities == 0u) {
        client->config.capabilities =
            H2_LOADER_CAP_STATUS |
            H2_LOADER_CAP_RESTART |
            H2_LOADER_CAP_ROLLBACK;
        if (client->config.disk != NULL) {
            client->config.capabilities |= H2_LOADER_CAP_COREDUMP;
        }
    }
    return H2_PAL_OK;
}

int h2_loader_app_client_restart(h2_loader_app_client_t *client) {
    if (client == NULL || client->config.power == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_pal_power_reboot(client->config.power, client->config.reboot_reason);
}

int h2_loader_app_client_return_to_loader(h2_loader_app_client_t *client) {
    int rc = prepare_return_to_loader(client);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_pal_power_reboot(client->config.power, client->config.reboot_reason);
}

int h2_loader_app_client_start_return_console(
    const h2_loader_app_client_return_console_config_t *config) {
    h2_loader_app_client_return_console_t *console;
    h2_pal_task_t *task = NULL;
    h2_pal_task_options_t options;

    if (config == NULL || config->client == NULL || config->task == NULL ||
        config->read_byte == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->client->return_console_task != NULL ||
        config->client->return_console_private != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    console = h2_pal_mem_alloc(
        config->client->config.allocator, sizeof(*console));
    if (console == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(console, 0, sizeof(*console));
    console->client = config->client;
    console->read_user = config->read_user;
    console->read_byte = config->read_byte;
    console->write_user = config->write_user;
    console->write = config->write != NULL ? config->write : stdout_write;
    atomic_init(&console->stop_requested, false);

    options.name = config->task_name != NULL ? config->task_name : "h2loader/appcmd";
    options.min_stack_size = config->stack_size != 0u ? config->stack_size : H2_LOADER_APP_CLIENT_MIN_STACK_SIZE;
    if (options.min_stack_size < H2_LOADER_APP_CLIENT_MIN_STACK_SIZE) {
        options.min_stack_size = H2_LOADER_APP_CLIENT_MIN_STACK_SIZE;
    }

    int rc = h2_pal_task_start(
        config->task,
        &options,
        return_console_task,
        console,
        &task);
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(config->client->config.allocator, console);
        return rc;
    }
    config->client->return_console_task_api = config->task;
    config->client->return_console_task = task;
    config->client->return_console_private = console;
    return H2_PAL_OK;
}

static int finish_return_console(
    h2_loader_app_client_t *client,
    bool request_stop) {
    if (client == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (client->return_console_task == NULL ||
        client->return_console_task_api == NULL ||
        client->return_console_private == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_loader_app_client_return_console_t *console =
        (h2_loader_app_client_return_console_t *)client->return_console_private;
    if (request_stop) {
        atomic_store_explicit(&console->stop_requested, true, memory_order_release);
    }
    int rc = h2_pal_task_join(
        client->return_console_task_api,
        client->return_console_task);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_pal_mem_free(client->config.allocator, console);
    client->return_console_task_api = NULL;
    client->return_console_task = NULL;
    client->return_console_private = NULL;
    return H2_PAL_OK;
}

int h2_loader_app_client_join_return_console(h2_loader_app_client_t *client) {
    return finish_return_console(client, false);
}

int h2_loader_app_client_stop_return_console(h2_loader_app_client_t *client) {
    return finish_return_console(client, true);
}
