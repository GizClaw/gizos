#include "h2_h2loader_cli_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define H2_H2LOADER_CLI_DEVICE_ARGV_CAPACITY 8u

typedef struct open_source {
    const h2_pal_fs_api_t *fs;
    h2_pal_fs_file_t *file;
} open_source_t;

static h2_pal_result_t cli_managed_connect(
    void *user,
    h2_h2loader_host_status_t *out_status) {
    return h2_h2loader_cli_transport_connect(user, out_status);
}

static h2_pal_result_t cli_managed_stage(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    return h2_h2loader_cli_transport_stage(
        user, asset, read_payload, payload_user, is_cancelled, cancel_user,
        on_progress, progress_user);
}

static h2_pal_result_t cli_managed_disconnect(void *user) {
    return h2_h2loader_cli_transport_disconnect(user);
}

static h2_pal_result_t cli_managed_rediscover(void *user) {
    return h2_h2loader_cli_transport_rediscover(user);
}

static const h2_h2loader_host_managed_transport_vtable_t
    cli_stage_transport_vtable = {
        .connect = cli_managed_connect,
        .stage = cli_managed_stage,
        .disconnect = cli_managed_disconnect,
        .rediscover = cli_managed_rediscover,
    };

void h2_h2loader_cli_send_progress(
    void *user,
    uint64_t acknowledged_bytes,
    uint64_t total_bytes) {
    h2_h2loader_cli_send_progress_t *progress = user;
    uint64_t now_ms = 0u;
    double percent;
    double rate_bps = 0.0;
    uint32_t whole_percent;

    if (progress == NULL || progress->context == NULL || total_bytes == 0u) {
        return;
    }
    if (acknowledged_bytes > total_bytes) {
        acknowledged_bytes = total_bytes;
    }
    percent = 100.0 * (double)acknowledged_bytes / (double)total_bytes;
    whole_percent = (uint32_t)percent;
    if (h2_pal_time_get_monotonic_ms(
            progress->context->runtime->time, &now_ms) != H2_PAL_OK) {
        now_ms = progress->start_ms;
    }
    if (progress->reported && acknowledged_bytes != total_bytes &&
        whole_percent < progress->last_percent + 5u &&
        now_ms - progress->last_report_ms < 1000u) {
        return;
    }
    if (now_ms > progress->start_ms) {
        rate_bps = 1000.0 * (double)acknowledged_bytes /
            (double)(now_ms - progress->start_ms);
    }
    (void)h2_h2loader_cli_output(
        progress->context,
        H2_H2LOADER_CLI_STREAM_STDOUT,
        "H2_LOADER_SEND state=progress acked=%llu total=%llu "
        "percent=%.1f rate_bps=%.0f\n",
        (unsigned long long)acknowledged_bytes,
        (unsigned long long)total_bytes,
        percent,
        rate_bps);
    progress->last_report_ms = now_ms;
    progress->last_percent = whole_percent;
    progress->reported = 1u;
}

static const char help_text[] =
    "usage: h2loader [--port ENDPOINT] [--transport iostreamikcp|bleikcp]\n"
    "                [--ready MARKER]\n"
    "                [--wait-timeout SECONDS] [--read-timeout SECONDS]\n"
    "                [--post-delay SECONDS] [--no-ble] COMMAND ...\n\n"
    "commands: package golden check scan status stats memory send send-url\n"
    "          stage hold wifi reboot reboot-loader restart restart-monitor\n"
    "          rollback upgrade coredump bleikcp-speed\n\n"
    "wifi:     wifi scan [--limit <1-16>] [--timeout-ms <1-30000>]\n"
    "          wifi connect <ssid> <password>\n"
    "          wifi disconnect\n";

static int parse_seconds(const char *value, uint32_t *out_ms) {
    char *end = NULL;
    double parsed;
    if (value == NULL || value[0] == '\0' || value[0] == '-') return 0;
    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0.0 || parsed > 3600.0) {
        return 0;
    }
    *out_ms = (uint32_t)(parsed * 1000.0);
    return *out_ms != 0u;
}

static int parse_nonnegative_seconds(const char *value, uint32_t *out_ms) {
    char *end = NULL;
    double parsed;
    if (value == NULL || value[0] == '\0' || value[0] == '-') return 0;
    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < 0.0 || parsed > 3600.0) {
        return 0;
    }
    *out_ms = (uint32_t)(parsed * 1000.0);
    return parsed == 0.0 || *out_ms != 0u;
}

static int parse_global(
    int argc,
    const char *const *argv,
    h2_h2loader_cli_options_t *out) {
    memset(out, 0, sizeof(*out));
    out->transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL;
    out->wait_timeout_ms = 10000u;
    out->read_timeout_ms = 2000u;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            if (out->no_ble &&
                out->transport == H2_H2LOADER_HOST_TRANSPORT_BLE) {
                return 0;
            }
            if (out->transport == H2_H2LOADER_HOST_TRANSPORT_BLE &&
                (out->ready_marker != NULL || out->post_delay_ms != 0u)) {
                return 0;
            }
            out->command_index = i;
            return 1;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            out->command_index = i;
            return 1;
        }
        if (strcmp(argv[i], "--no-ble") == 0) {
            out->no_ble = 1;
            continue;
        }
        if (i + 1 >= argc) return 0;
        if (strcmp(argv[i], "--port") == 0) out->port = argv[++i];
        else if (strcmp(argv[i], "--transport") == 0) {
            const char *transport = argv[++i];
            if (strcmp(transport, "iostreamikcp") == 0) {
                out->transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL;
            } else if (strcmp(transport, "bleikcp") == 0) {
                out->transport = H2_H2LOADER_HOST_TRANSPORT_BLE;
            } else return 0;
        } else if (strcmp(argv[i], "--ready") == 0) {
            out->ready_marker = argv[++i];
        } else if (strcmp(argv[i], "--wait-timeout") == 0) {
            if (!parse_seconds(argv[++i], &out->wait_timeout_ms)) return 0;
        } else if (strcmp(argv[i], "--read-timeout") == 0) {
            if (!parse_seconds(argv[++i], &out->read_timeout_ms)) return 0;
        } else if (strcmp(argv[i], "--post-delay") == 0) {
            if (!parse_nonnegative_seconds(argv[++i], &out->post_delay_ms)) {
                return 0;
            }
        } else return 0;
    }
    return 0;
}

const h2_pal_ble_host_api_t *h2_h2loader_cli_acquire_ble(
    h2_h2loader_cli_context_t *context) {
    if (context->ble_disabled) return NULL;
    if (context->runtime->ble_host == NULL &&
        context->config->acquire_ble != NULL) {
        context->runtime->ble_host =
            context->config->acquire_ble(context->config->ble_user);
    }
    return context->runtime->ble_host;
}

static h2_pal_result_t command_output(void *user, const uint8_t *data, size_t len) {
    h2_h2loader_cli_context_t *context = user;
    return h2_h2loader_cli_output_bytes(
        context, H2_H2LOADER_CLI_STREAM_STDOUT, data, len);
}

static h2_pal_result_t transport_log(void *user, const uint8_t *data, size_t len) {
    return command_output(user, data, len);
}

static h2_pal_result_t file_read(
    void *user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read) {
    open_source_t *source = user;
    h2_pal_result_t result = h2_pal_fs_seek(source->fs, source->file, offset);
    return result == H2_PAL_OK
        ? h2_pal_fs_read(source->fs, source->file, out, out_size, out_read)
        : result;
}

static int scan_command(h2_h2loader_cli_context_t *context, int argc, const char *const *argv) {
    h2_h2loader_host_candidate_t candidates[32];
    h2_h2loader_host_scan_result_t result;
    uint32_t timeout = 10000u;
    if (argc != 0) {
        if (argc != 2 || strcmp(argv[0], "--probe-timeout") != 0 ||
            !parse_seconds(argv[1], &timeout)) return H2_H2LOADER_CLI_EXIT_USAGE;
    }
    h2_h2loader_host_scan_config_t scan = {
        .serial = context->config->serial,
        .ble = h2_h2loader_cli_acquire_ble(context),
        .sync = context->runtime->sync,
        .time = context->runtime->time,
        .ble_timeout_ms = timeout,
        .candidates = candidates,
        .candidate_capacity = sizeof(candidates) / sizeof(candidates[0]),
    };
    h2_pal_result_t rc = h2_h2loader_host_scan(&scan, &result);
    if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
    rc = h2_h2loader_cli_output(
        context, H2_H2LOADER_CLI_STREAM_STDOUT, "{\n  \"devices\": [");
    if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
    for (size_t i = 0u; i < result.count; ++i) {
        rc = h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDOUT,
            "%s\n    {\"transport\": \"%s\", \"port\": ",
            i == 0u ? "" : ",",
            candidates[i].transport == H2_H2LOADER_HOST_TRANSPORT_SERIAL
                ? "iostreamikcp"
                : "bleikcp");
        if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
        rc = h2_h2loader_cli_output_json_string(
            context, H2_H2LOADER_CLI_STREAM_STDOUT, candidates[i].port_id);
        if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
        rc = h2_h2loader_cli_output(
            context, H2_H2LOADER_CLI_STREAM_STDOUT, ", \"endpoint\": ");
        if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
        rc = h2_h2loader_cli_output_json_string(
            context, H2_H2LOADER_CLI_STREAM_STDOUT, candidates[i].endpoint);
        if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
        rc = h2_h2loader_cli_output(
            context, H2_H2LOADER_CLI_STREAM_STDOUT, ", \"board\": ");
        if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
        rc = h2_h2loader_cli_output_json_string(
            context,
            H2_H2LOADER_CLI_STREAM_STDOUT,
            candidates[i].advertised_board);
        if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
        rc = h2_h2loader_cli_output(
            context, H2_H2LOADER_CLI_STREAM_STDOUT, "}");
        if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    rc = h2_h2loader_cli_output(
        context, H2_H2LOADER_CLI_STREAM_STDOUT, "\n  ]\n}\n");
    return rc == H2_PAL_OK
        ? H2_H2LOADER_CLI_EXIT_OK
        : H2_H2LOADER_CLI_EXIT_RUNTIME;
}

static h2_h2loader_host_command_t command_kind(int argc, const char *const *argv) {
    if (argc == 1 && strcmp(argv[0], "status") == 0) return H2_H2LOADER_HOST_COMMAND_STATUS;
    if (argc == 1 && strcmp(argv[0], "stats") == 0) return H2_H2LOADER_HOST_COMMAND_STATS;
    if (argc == 1 && strcmp(argv[0], "memory") == 0) return H2_H2LOADER_HOST_COMMAND_MEMORY;
    if (argc == 1 && strcmp(argv[0], "restart") == 0) return H2_H2LOADER_HOST_COMMAND_APP_RESTART;
    if (argc == 1 && strcmp(argv[0], "rollback") == 0) return H2_H2LOADER_HOST_COMMAND_APP_ROLLBACK;
    if (argc == 1 && strcmp(argv[0], "reboot") == 0) return H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_APP;
    if (argc == 1 && strcmp(argv[0], "reboot-loader") == 0) return H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_LOADER;
    if (argc == 1 && strcmp(argv[0], "upgrade") == 0) return H2_H2LOADER_HOST_COMMAND_LOADER_UPGRADE;
    if (argc == 2 && strcmp(argv[0], "stage") == 0 && strcmp(argv[1], "abort") == 0) return H2_H2LOADER_HOST_COMMAND_STAGE_ABORT;
    if (argc == 2 && strcmp(argv[0], "hold") == 0 && strcmp(argv[1], "on") == 0) return H2_H2LOADER_HOST_COMMAND_HOLD_ON;
    if (argc == 2 && strcmp(argv[0], "hold") == 0 && strcmp(argv[1], "off") == 0) return H2_H2LOADER_HOST_COMMAND_HOLD_OFF;
    if (argc == 2 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "disconnect") == 0) return H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT;
    if (argc >= 2 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "scan") == 0) return H2_H2LOADER_HOST_COMMAND_WIFI_SCAN;
    if (argc == 2 && strcmp(argv[0], "coredump") == 0 && strcmp(argv[1], "status") == 0) return H2_H2LOADER_HOST_COMMAND_COREDUMP_STATUS;
    if (argc == 2 && strcmp(argv[0], "coredump") == 0 && strcmp(argv[1], "dump") == 0) return H2_H2LOADER_HOST_COMMAND_COREDUMP_DUMP;
    if (argc == 2 && strcmp(argv[0], "coredump") == 0 && strcmp(argv[1], "erase") == 0) return H2_H2LOADER_HOST_COMMAND_COREDUMP_ERASE;
    if (argc == 4 && strcmp(argv[0], "wifi") == 0 && strcmp(argv[1], "connect") == 0) return H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT;
    return 0;
}

static int parse_wifi_scan_options(
    int argc,
    const char *const *argv,
    uint32_t *out_limit,
    uint32_t *out_timeout_ms) {
    int saw_limit = 0;
    int saw_timeout = 0;

    *out_limit = H2_H2LOADER_HOST_WIFI_SCAN_DEFAULT_LIMIT;
    *out_timeout_ms = H2_H2LOADER_HOST_WIFI_SCAN_DEFAULT_TIMEOUT_MS;
    for (int i = 2; i < argc; i += 2) {
        char *end = NULL;
        unsigned long value;
        if (i + 1 >= argc || argv[i + 1][0] == '\0') return 0;
        errno = 0;
        value = strtoul(argv[i + 1], &end, 10);
        if (errno != 0 || end == argv[i + 1] || *end != '\0' || value == 0u) {
            return 0;
        }
        if (strcmp(argv[i], "--limit") == 0 && !saw_limit &&
            value <= H2_H2LOADER_HOST_WIFI_SCAN_MAX_LIMIT) {
            *out_limit = (uint32_t)value;
            saw_limit = 1;
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && !saw_timeout &&
            value <= H2_H2LOADER_HOST_WIFI_SCAN_MAX_TIMEOUT_MS) {
            *out_timeout_ms = (uint32_t)value;
            saw_timeout = 1;
        } else {
            return 0;
        }
    }
    return 1;
}

static int device_command(
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    int argc,
    const char *const *argv) {
    h2_h2loader_cli_transport_t transport;
    h2_h2loader_host_status_t status = {0};
    h2_h2loader_host_command_request_t request;
    h2_h2loader_host_command_result_t result = {0};
    h2_h2loader_host_command_t kind = command_kind(argc, argv);
    uint32_t wifi_scan_limit = 0u;
    uint32_t wifi_scan_timeout_ms = 0u;
    h2_pal_result_t rc;
    if (kind == H2_H2LOADER_HOST_COMMAND_WIFI_SCAN &&
        !parse_wifi_scan_options(
            argc, argv, &wifi_scan_limit, &wifi_scan_timeout_ms)) {
        return H2_H2LOADER_CLI_EXIT_USAGE;
    }
    if (kind == 0 || options->port == NULL) return H2_H2LOADER_CLI_EXIT_USAGE;
    h2_h2loader_cli_transport_init(
        &transport, context, options, options->read_timeout_ms);
    rc = h2_h2loader_cli_transport_connect(&transport, &status);
    request = (h2_h2loader_host_command_request_t){
        .command = kind,
        .status = &status,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = command_output,
        .output_user = context,
    };
    if (kind == H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT) {
        request.ssid = argv[2];
        request.password = argv[3];
    } else if (kind == H2_H2LOADER_HOST_COMMAND_WIFI_SCAN) {
        request.wifi_scan_limit = wifi_scan_limit;
        request.wifi_scan_timeout_ms = wifi_scan_timeout_ms;
    }
    if (rc == H2_PAL_OK) rc = h2_h2loader_cli_transport_execute(
        &transport, &request, &result);
    (void)h2_h2loader_cli_transport_disconnect(&transport);
    if (rc != H2_PAL_OK || result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
        h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDERR,
            "h2loader: command failed code=%d terminal=%d\n", rc, result.terminal);
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    return H2_H2LOADER_CLI_EXIT_OK;
}

static int upgrade_command(
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    int argc) {
    h2_h2loader_cli_transport_t transport;
    h2_h2loader_host_status_t status = {0};
    h2_h2loader_host_upgrade_tracker_t tracker;
    h2_h2loader_host_command_result_t result = {0};
    h2_pal_result_t rc;
    if (argc != 0 || options->port == NULL) {
        return H2_H2LOADER_CLI_EXIT_USAGE;
    }
    h2_h2loader_cli_transport_init(
        &transport, context, options, options->read_timeout_ms);
    rc = h2_h2loader_cli_transport_connect(&transport, &status);
    if (rc == H2_PAL_OK) {
        rc = h2_h2loader_host_upgrade_tracker_init(&status, &tracker);
    }
    if (rc == H2_PAL_OK) {
        h2_h2loader_host_command_request_t request = {
            .command = H2_H2LOADER_HOST_COMMAND_LOADER_UPGRADE,
            .status = &status,
            .is_cancelled = context->config->is_cancelled,
            .cancel_user = context->config->cancel_user,
            .on_output = command_output,
            .output_user = context,
        };
        rc = h2_h2loader_cli_transport_execute(
            &transport, &request, &result);
        if (rc == H2_PAL_OK &&
            result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
            rc = H2_PAL_ERR_IO;
        }
    }
    (void)h2_h2loader_cli_transport_disconnect(&transport);
    if (rc != H2_PAL_OK) {
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    rc = H2_PAL_ERR_TIMEOUT;
    for (uint32_t attempt = 0u; attempt < 60u; ++attempt) {
        if (context->config->is_cancelled != NULL &&
            context->config->is_cancelled(context->config->cancel_user)) {
            rc = H2_PAL_EXIT;
            break;
        }
        rc = h2_pal_time_sleep_ms(context->runtime->time, 1000u);
        if (rc != H2_PAL_OK) {
            break;
        }
        rc = h2_h2loader_cli_transport_rediscover(&transport);
        if (rc == H2_PAL_OK) {
            rc = h2_h2loader_cli_transport_connect(&transport, &status);
        }
        if (rc == H2_PAL_OK) {
            rc = h2_h2loader_host_upgrade_tracker_observe(&tracker, &status);
        }
        (void)h2_h2loader_cli_transport_disconnect(&transport);
        if (rc == H2_PAL_OK) {
            h2_h2loader_cli_output(
                context,
                H2_H2LOADER_CLI_STREAM_STDOUT,
                "H2_LOADER_UPGRADE result=OK transition=reconnected "
                "active_version=%s\n",
                status.active_version);
            return H2_H2LOADER_CLI_EXIT_OK;
        }
        if (rc == H2_PAL_ERR_INVALID_STATE) {
            break;
        }
        rc = H2_PAL_ERR_TIMEOUT;
    }
    return H2_H2LOADER_CLI_EXIT_RUNTIME;
}

static int restart_monitor_command(
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    int argc) {
    h2_h2loader_cli_transport_t transport;
    h2_h2loader_host_status_t status = {0};
    h2_h2loader_host_command_result_t result = {0};
    h2_pal_result_t rc;
    if (argc != 0 || options->port == NULL) return H2_H2LOADER_CLI_EXIT_USAGE;
    if (options->transport == H2_H2LOADER_HOST_TRANSPORT_BLE) {
        h2_h2loader_cli_output(
            context, H2_H2LOADER_CLI_STREAM_STDERR,
            "h2loader: restart-monitor requires iostreamikcp log transport\n");
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    h2_h2loader_cli_transport_init(
        &transport, context, options, options->read_timeout_ms);
    transport.on_log = transport_log;
    transport.log_user = context;
    rc = h2_h2loader_cli_transport_connect(&transport, &status);
    h2_h2loader_host_command_request_t request = {
        .command = H2_H2LOADER_HOST_COMMAND_APP_RESTART,
        .status = &status,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_output = command_output,
        .output_user = context,
    };
    if (rc == H2_PAL_OK) rc = h2_h2loader_cli_transport_execute(
        &transport, &request, &result);
    if (rc == H2_PAL_OK && result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
        rc = h2_h2loader_cli_transport_monitor_logs(
            &transport, context->config->is_cancelled,
            context->config->cancel_user);
    }
    (void)h2_h2loader_cli_transport_disconnect(&transport);
    return rc == H2_PAL_EXIT ? H2_H2LOADER_CLI_EXIT_OK :
        (rc == H2_PAL_OK ? H2_H2LOADER_CLI_EXIT_OK : H2_H2LOADER_CLI_EXIT_RUNTIME);
}

static int send_source_failure(
    h2_h2loader_cli_context_t *context,
    const char *step,
    const char *path,
    h2_pal_result_t rc) {
    const char *hint = strcmp(step, "inspect") == 0
        ? "file is not a valid update package"
        : "path must resolve to a readable file inside a PAL filesystem mount "
          "(native CLI mounts /tmp and the user home root only)";
    h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDERR,
        "h2loader: send failed step=%s file=%s code=%d (%s)\n",
        step, path, (int)rc, hint);
    return H2_H2LOADER_CLI_EXIT_RUNTIME;
}

static int send_command(
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    int argc,
    const char *const *argv) {
    open_source_t source = {.fs = context->runtime->fs};
    h2_h2loader_host_catalog_entry_t asset = {0};
    h2_h2loader_host_status_t final_status = {0};
    h2_h2loader_cli_transport_t transport;
    h2_h2loader_cli_send_progress_t progress = {
        .context = context,
    };
    uint64_t size = 0u;
    h2_pal_result_t rc;
    if (options->port == NULL || argc != 2 || strcmp(argv[0], "--file") != 0) {
        return H2_H2LOADER_CLI_EXIT_USAGE;
    }
    if (context->runtime->fs == NULL) {
        h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDERR,
            "h2loader: send failed step=fs file=%s code=%d "
            "(filesystem unavailable)\n", argv[1], (int)H2_PAL_ERR_UNSUPPORTED);
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    {
        h2_pal_fs_stat_t stat_value;
        rc = h2_pal_fs_stat(context->runtime->fs, argv[1], &stat_value);
        if (rc == H2_PAL_OK && stat_value.is_dir) rc = H2_PAL_ERR_INVALID_ARG;
        if (rc == H2_PAL_OK) size = stat_value.size;
        if (rc != H2_PAL_OK) {
            return send_source_failure(context, "stat", argv[1], rc);
        }
    }
    rc = h2_pal_fs_open(
        context->runtime->fs, argv[1], H2_PAL_FS_OPEN_READ, &source.file);
    if (rc != H2_PAL_OK) {
        return send_source_failure(context, "open", argv[1], rc);
    }
    h2_h2loader_host_package_inspect_config_t inspect = {
        .allocator = context->runtime->mem,
        .read_payload = file_read,
        .payload_user = &source,
        .payload_bytes = size,
    };
    rc = h2_h2loader_host_package_inspect(&inspect, &asset);
    if (rc != H2_PAL_OK) {
        (void)h2_pal_fs_close(context->runtime->fs, source.file);
        return send_source_failure(context, "inspect", argv[1], rc);
    }
    h2_h2loader_cli_transport_init(
        &transport, context, options, options->read_timeout_ms);
    h2_h2loader_host_managed_operation_config_t operation = {
        .time = context->runtime->time,
        .transport = {
            .user = &transport,
            .vtable = &cli_stage_transport_vtable,
        },
        .asset = &asset,
        .read_payload = file_read,
        .payload_user = &source,
        .is_cancelled = context->config->is_cancelled,
        .cancel_user = context->config->cancel_user,
        .on_progress = h2_h2loader_cli_send_progress,
        .progress_user = &progress,
        .reconnect_delay_ms = 250u,
        .reconnect_attempts = 40u,
    };
    (void)h2_pal_time_get_monotonic_ms(
        context->runtime->time, &progress.start_ms);
    progress.last_report_ms = progress.start_ms;
    if (rc == H2_PAL_OK) {
        rc = h2_h2loader_host_stage_operation_run(&operation, &final_status);
    }
    (void)h2_h2loader_cli_transport_disconnect(&transport);
    if (source.file != NULL) {
        (void)h2_pal_fs_close(context->runtime->fs, source.file);
        source.file = NULL;
    }
    if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
    h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDOUT,
        "H2_LOADER_SEND result=OK bytes=%llu checksum=%s\n",
        (unsigned long long)asset.bytes, asset.sha256);
    return H2_H2LOADER_CLI_EXIT_OK;
}

static int check_command(h2_h2loader_cli_context_t *context) {
    static const char *capabilities[] = {
        "memory", "time", "filesystem", "serial", "ble",
    };
    const void *apis[] = {
        context->runtime->mem,
        context->runtime->time,
        context->runtime->fs,
        context->config->serial,
    };
    /* BLE availability is reported without starting the BLE Host so check
     * stays safe on hosts that deny Bluetooth access to this process. */
    int ble_available = !context->ble_disabled &&
        (context->runtime->ble_host != NULL ||
            context->config->acquire_ble != NULL);
    int required_available = 1;
    for (size_t i = 0u; i < sizeof(capabilities) / sizeof(capabilities[0]); ++i) {
        int available = i < 4u ? apis[i] != NULL : ble_available;
        h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDOUT,
            "H2_LOADER_CHECK capability=%s available=%d\n",
            capabilities[i], available);
        if (i < 4u && !available) required_available = 0;
    }
    h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDOUT,
        "H2_LOADER_CHECK result=%s\n",
        required_available ? "OK" : "incomplete");
    return required_available
        ? H2_H2LOADER_CLI_EXIT_OK
        : H2_H2LOADER_CLI_EXIT_RUNTIME;
}

int h2_h2loader_cli_main(h2_runtime_t *runtime, const h2_h2loader_cli_config_t *config) {
    h2_h2loader_cli_options_t options;
    h2_h2loader_cli_context_t context = {.runtime = runtime, .config = config};
    const char *command;
    int argc;
    const char *const *argv;
    if (runtime == NULL || runtime->mem == NULL || runtime->time == NULL ||
        config == NULL || config->argc < 1 || config->argv == NULL ||
        config->stdout_io == NULL || config->stderr_io == NULL) {
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    if (!parse_global(config->argc, config->argv, &options)) {
        h2_h2loader_cli_output(&context, H2_H2LOADER_CLI_STREAM_STDERR, "%s", help_text);
        return H2_H2LOADER_CLI_EXIT_USAGE;
    }
    context.ble_disabled = options.no_ble;
    command = config->argv[options.command_index];
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        h2_h2loader_cli_output(&context, H2_H2LOADER_CLI_STREAM_STDOUT, "%s", help_text);
        return H2_H2LOADER_CLI_EXIT_OK;
    }
    argc = config->argc - options.command_index - 1;
    argv = &config->argv[options.command_index + 1];
    if (strcmp(command, "package") == 0) return h2_h2loader_cli_package_command(&context, argc, argv, 0);
    if (strcmp(command, "golden") == 0) return h2_h2loader_cli_package_command(&context, argc, argv, 1);
    if (strcmp(command, "check") == 0 && argc == 0) return check_command(&context);
    if (strcmp(command, "scan") == 0) return scan_command(&context, argc, argv);
    if (strcmp(command, "send") == 0) return send_command(&context, &options, argc, argv);
    if (strcmp(command, "send-url") == 0) {
        return h2_h2loader_cli_server_command(&context, &options, argc, argv);
    }
    if (strcmp(command, "restart-monitor") == 0) return restart_monitor_command(&context, &options, argc);
    if (strcmp(command, "upgrade") == 0) return upgrade_command(&context, &options, argc);
    if (strcmp(command, "bleikcp-speed") == 0)
        return h2_h2loader_cli_bleikcp_speed_command(&context, argc, argv);
    {
        const char *parts[H2_H2LOADER_CLI_DEVICE_ARGV_CAPACITY] = {command};
        int count = 1;
        while (count < (int)H2_H2LOADER_CLI_DEVICE_ARGV_CAPACITY && count - 1 < argc) {
            parts[count] = argv[count - 1];
            ++count;
        }
        return device_command(&context, &options, count, parts);
    }
}
