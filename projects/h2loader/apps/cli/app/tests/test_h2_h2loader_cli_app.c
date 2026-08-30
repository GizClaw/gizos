#include "h2_h2loader_cli_app.h"
#include "h2_h2loader_cli_internal.h"

#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct fake_output {
    char bytes[16384];
    size_t len;
    h2_pal_result_t write_result;
    h2_pal_result_t flush_result;
} fake_output_t;

typedef struct fake_time {
    uint64_t now_ms;
} fake_time_t;

static h2_pal_result_t fake_monotonic_ms(void *user, uint64_t *out_ms) {
    fake_time_t *time = user;
    *out_ms = time->now_ms;
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t time_vtable = {
    .get_monotonic_ms = fake_monotonic_ms,
};

static h2_pal_result_t fake_write(
    void *user, const void *data, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
    fake_output_t *output = user;
    (void)timeout_ms;
    if (output->write_result != H2_PAL_OK) return output->write_result;
    if (data == NULL || out_written == NULL ||
        len > sizeof(output->bytes) - output->len - 1u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(&output->bytes[output->len], data, len);
    output->len += len;
    output->bytes[output->len] = '\0';
    *out_written = len;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_flush(void *user) {
    fake_output_t *output = user;
    return output->flush_result;
}

static const h2_command_io_vtable_t output_vtable = {
    .write = fake_write,
    .flush = fake_flush,
};

typedef struct fake_ble_source {
    const h2_pal_ble_host_api_t *api;
    unsigned calls;
} fake_ble_source_t;

typedef struct fake_scan_probe {
    const char *ports[8];
    uint32_t timeouts[8];
    size_t calls;
} fake_scan_probe_t;

typedef struct fake_scan_cancel {
    size_t checks;
    size_t cancel_after;
} fake_scan_cancel_t;

static int fake_scan_is_cancelled(void *user) {
    fake_scan_cancel_t *cancel = user;
    ++cancel->checks;
    return cancel->checks > cancel->cancel_after;
}

static h2_pal_result_t fake_scan_probe_candidate(
    void *user,
    const h2_h2loader_host_candidate_t *candidate,
    uint32_t timeout_ms,
    h2_h2loader_host_status_t *out_status) {
    fake_scan_probe_t *probe = user;
    probe->ports[probe->calls] = candidate->port_id;
    probe->timeouts[probe->calls] = timeout_ms;
    ++probe->calls;
    if (strcmp(candidate->port_id, "port-timeout") == 0) {
        return H2_PAL_ERR_TIMEOUT;
    }
    strcpy(out_status->board, "tiga");
    strcpy(out_status->target, "esp32s3");
    strcpy(out_status->active_version,
        strcmp(candidate->port_id, "port-a") == 0 ? "v1" : "v2");
    out_status->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
    out_status->boot_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
    return H2_PAL_OK;
}

static const h2_pal_ble_host_api_t *fake_acquire_ble(void *user) {
    fake_ble_source_t *source = user;
    ++source->calls;
    return source->api;
}

static int run_cli_with_ble(
    fake_output_t *output,
    int argc,
    const char *const *argv,
    fake_ble_source_t *ble_source) {
    h2_command_io_api_t io = {.user = output, .vtable = &output_vtable};
    h2_runtime_t runtime = {
        .mem = h2_pal_unsupported_mem_api(),
        .time = h2_pal_unsupported_time_api(),
        .fs = h2_pal_unsupported_fs_api(),
    };
    h2_h2loader_cli_config_t config = {
        .argc = argc,
        .argv = argv,
        .serial = h2_pal_unsupported_serial_host_api(),
        .stdout_io = &io,
        .stderr_io = &io,
        .acquire_ble = ble_source != NULL ? fake_acquire_ble : NULL,
        .ble_user = ble_source,
    };
    return h2_h2loader_cli_main(&runtime, &config);
}

static int run_cli(fake_output_t *output, int argc, const char *const *argv) {
    return run_cli_with_ble(output, argc, argv, NULL);
}

static void test_help_and_usage(void) {
    const char *help[] = {"h2loader", "-h"};
    const char *raw[] = {"h2loader", "--transport", "raw", "status"};
    const char *missing_port[] = {
        "h2loader", "wifi", "connect", "ssid", "secret",
    };
    const char *invalid_scan_limit[] = {
        "h2loader", "wifi", "scan", "--limit", "0",
    };
    const char *duplicate_scan_timeout[] = {
        "h2loader", "wifi", "scan", "--timeout-ms", "10",
        "--timeout-ms", "20",
    };
    const char *disabled_ble[] = {
        "h2loader", "--no-ble", "--transport", "bleikcp",
        "--port", "1:001122334455", "status",
    };
    const char *serial_only_ble_option[] = {
        "h2loader", "--transport", "bleikcp", "--ready", "READY",
        "--port", "1:001122334455", "status",
    };
    const char *ble_monitor[] = {
        "h2loader", "--transport", "bleikcp", "--port",
        "1:001122334455", "monitor",
    };
    const char *ble_reboot_monitor[] = {
        "h2loader", "--transport", "bleikcp", "--port",
        "1:001122334455", "reboot", "app", "--monitor",
    };
    fake_output_t output = {0};

    assert(run_cli(&output, 2, help) == H2_H2LOADER_CLI_EXIT_OK);
    assert(strstr(output.bytes, "commands: package golden check scan") != NULL);
    assert(strstr(output.bytes, "restart-monitor") == NULL);
    assert(strstr(output.bytes, "monitor") != NULL);
    assert(strstr(output.bytes, "--transport iostreamikcp|bleikcp") != NULL);
    assert(strstr(output.bytes,
        "wifi:     wifi scan [--limit <1-16>] [--timeout-ms <1-30000>]\n"
        "          wifi connect <ssid> <password>\n"
        "          wifi disconnect\n") != NULL);
    assert(strstr(output.bytes, "secret") == NULL);

    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 7, disabled_ble) == H2_H2LOADER_CLI_EXIT_USAGE);
    assert(strstr(output.bytes, "usage: h2loader") != NULL);

    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 8, serial_only_ble_option) ==
        H2_H2LOADER_CLI_EXIT_USAGE);

    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 6, ble_monitor) == H2_H2LOADER_CLI_EXIT_RUNTIME);
    assert(strstr(output.bytes,
        "monitor requires iostreamikcp log transport") != NULL);

    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 8, ble_reboot_monitor) ==
        H2_H2LOADER_CLI_EXIT_RUNTIME);
    assert(strstr(output.bytes,
        "--monitor requires iostreamikcp log transport") != NULL);

    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 4, raw) == H2_H2LOADER_CLI_EXIT_USAGE);
    assert(strstr(output.bytes, "usage: h2loader") != NULL);

    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 5, missing_port) == H2_H2LOADER_CLI_EXIT_USAGE);
    assert(strstr(output.bytes, "secret") == NULL);

    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 5, invalid_scan_limit) ==
        H2_H2LOADER_CLI_EXIT_USAGE);

    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 7, duplicate_scan_timeout) ==
        H2_H2LOADER_CLI_EXIT_USAGE);
}

static void test_ble_transport_routes_the_shared_device_command(void) {
    const char *argv[] = {
        "h2loader", "--transport", "bleikcp", "--port",
        "1:001122334455", "status",
    };
    fake_output_t output = {0};
    fake_ble_source_t source = {
        .api = h2_pal_unsupported_ble_host_api(),
    };

    assert(run_cli_with_ble(&output, 6, argv, &source) ==
        H2_H2LOADER_CLI_EXIT_RUNTIME);
    assert(source.calls == 1u);
    assert(strstr(output.bytes, "usage: h2loader") == NULL);
    assert(strstr(output.bytes, "command failed") != NULL);
}

static void test_legacy_upgrade_is_rejected_before_connect(void) {
    const char *argv[] = {
        "h2loader", "--transport", "bleikcp", "--port",
        "1:001122334455", "upgrade",
    };
    fake_output_t output = {0};
    fake_ble_source_t source = {
        .api = h2_pal_unsupported_ble_host_api(),
    };

    assert(run_cli_with_ble(&output, 6, argv, &source) ==
        H2_H2LOADER_CLI_EXIT_USAGE);
    assert(source.calls == 0u);
}

static void test_check_reports_runtime_capabilities(void) {
    const char *argv[] = {"h2loader", "check"};
    fake_output_t output = {0};

    assert(run_cli(&output, 2, argv) == H2_H2LOADER_CLI_EXIT_OK);
    assert(strstr(output.bytes, "capability=memory available=1") != NULL);
    assert(strstr(output.bytes, "capability=filesystem available=1") != NULL);
    assert(strstr(output.bytes, "capability=ble available=0") != NULL);
    assert(strstr(output.bytes, "H2_LOADER_CHECK result=OK") != NULL);
}

static void test_ble_acquired_only_when_requested(void) {
    const char *help[] = {"h2loader", "--help"};
    const char *check[] = {"h2loader", "check"};
    const char *check_no_ble[] = {"h2loader", "--no-ble", "check"};
    const char *scan[] = {"h2loader", "scan"};
    const char *scan_no_ble[] = {"h2loader", "--no-ble", "scan"};
    fake_ble_source_t source = {0};
    fake_output_t output = {0};

    assert(run_cli_with_ble(&output, 2, help, &source) ==
        H2_H2LOADER_CLI_EXIT_OK);
    assert(source.calls == 0u);

    memset(&output, 0, sizeof(output));
    assert(run_cli_with_ble(&output, 2, check, &source) ==
        H2_H2LOADER_CLI_EXIT_OK);
    assert(source.calls == 0u);
    assert(strstr(output.bytes, "capability=ble available=1") != NULL);

    memset(&output, 0, sizeof(output));
    assert(run_cli_with_ble(&output, 3, check_no_ble, &source) ==
        H2_H2LOADER_CLI_EXIT_OK);
    assert(source.calls == 0u);
    assert(strstr(output.bytes, "capability=ble available=0") != NULL);

    memset(&output, 0, sizeof(output));
    assert(run_cli_with_ble(&output, 2, scan, &source) ==
        H2_H2LOADER_CLI_EXIT_OK);
    assert(source.calls == 1u);

    memset(&output, 0, sizeof(output));
    source.calls = 0u;
    assert(run_cli_with_ble(&output, 3, scan_no_ble, &source) ==
        H2_H2LOADER_CLI_EXIT_OK);
    assert(source.calls == 0u);
}

static void test_bleikcp_speed_ble_lifecycle_boundaries(void) {
    const char *speed_usage[] = {"h2loader", "bleikcp-speed", "--duration"};
    const char *speed_valid[] = {
        "h2loader", "bleikcp-speed", "--json-out", "/tmp/speed.json",
    };
    const char *speed_no_ble[] = {
        "h2loader", "--no-ble", "bleikcp-speed",
        "--json-out", "/tmp/speed.json",
    };
    fake_ble_source_t source = {0};
    fake_output_t output = {0};

    assert(run_cli_with_ble(&output, 3, speed_usage, &source) ==
        H2_H2LOADER_CLI_EXIT_USAGE);
    assert(source.calls == 0u);

    memset(&output, 0, sizeof(output));
    assert(run_cli_with_ble(&output, 4, speed_valid, &source) ==
        H2_H2LOADER_CLI_EXIT_RUNTIME);
    assert(source.calls == 1u);
    assert(strstr(output.bytes,
        "bleikcp-speed unsupported: no real BLE Host PAL") != NULL);

    memset(&output, 0, sizeof(output));
    source.calls = 0u;
    assert(run_cli_with_ble(&output, 5, speed_no_ble, &source) ==
        H2_H2LOADER_CLI_EXIT_RUNTIME);
    assert(source.calls == 0u);
    assert(strstr(output.bytes,
        "bleikcp-speed unsupported: no real BLE Host PAL") != NULL);
}

static void test_scan_reports_output_failures(void) {
    const char *argv[] = {"h2loader", "scan"};
    fake_output_t output = {.write_result = H2_PAL_ERR_IO};

    assert(run_cli(&output, 2, argv) == H2_H2LOADER_CLI_EXIT_RUNTIME);
    memset(&output, 0, sizeof(output));
    output.flush_result = H2_PAL_ERR_IO;
    assert(run_cli(&output, 2, argv) == H2_H2LOADER_CLI_EXIT_RUNTIME);
}

static void test_scan_binds_live_status_to_exact_serial_port(void) {
    fake_output_t output = {0};
    h2_command_io_api_t io = {.user = &output, .vtable = &output_vtable};
    h2_h2loader_cli_config_t config = {
        .stdout_io = &io,
        .stderr_io = &io,
    };
    h2_h2loader_cli_context_t context = {.config = &config};
    h2_h2loader_host_candidate_t candidates[4] = {
        {
            .transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL,
            .port_id = "port-a",
            .endpoint = "/dev/cu.a",
        },
        {
            .transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL,
            .port_id = "port-timeout",
            .endpoint = "/dev/cu.timeout",
        },
        {
            .transport = H2_H2LOADER_HOST_TRANSPORT_BLE,
            .endpoint = "1:001122334455",
            .advertised_board = "advertised-only",
        },
        {
            .transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL,
            .port_id = "port-b",
            .endpoint = "/dev/cu.b",
        },
    };
    fake_scan_probe_t probe = {0};

    assert(h2_h2loader_cli_scan_candidates(
               &context,
               candidates,
               sizeof(candidates) / sizeof(candidates[0]),
               5000u,
               fake_scan_probe_candidate,
               &probe) == H2_H2LOADER_CLI_EXIT_OK);
    assert(probe.calls == 3u);
    assert(strcmp(probe.ports[0], "port-a") == 0);
    assert(strcmp(probe.ports[1], "port-timeout") == 0);
    assert(strcmp(probe.ports[2], "port-b") == 0);
    assert(probe.timeouts[0] == 5000u && probe.timeouts[1] == 5000u &&
           probe.timeouts[2] == 5000u);
    assert(strstr(
        output.bytes,
        "\"port\": \"port-a\", \"endpoint\": \"/dev/cu.a\", "
        "\"probe_result\": \"ok\", \"probe_code\": 0, "
        "\"board\": \"tiga\"") != NULL);
    assert(strstr(
        output.bytes,
        "\"port\": \"port-timeout\", "
        "\"endpoint\": \"/dev/cu.timeout\", "
        "\"probe_result\": \"error\", \"probe_code\": -6, "
        "\"board\": \"\", \"target\": \"\"") != NULL);
    assert(strstr(
        output.bytes,
        "\"transport\": \"bleikcp\", \"port\": \"\", "
        "\"endpoint\": \"1:001122334455\", "
        "\"board\": \"advertised-only\"") != NULL);
    assert(strstr(
        output.bytes,
        "\"port\": \"port-b\"") != NULL);
    assert(strstr(
        output.bytes,
        "\"active_version\": \"v2\"") != NULL);

    memset(&output, 0, sizeof(output));
    memset(&probe, 0, sizeof(probe));
    fake_scan_cancel_t cancel = {.cancel_after = 1u};
    config.is_cancelled = fake_scan_is_cancelled;
    config.cancel_user = &cancel;
    assert(h2_h2loader_cli_scan_candidates(
               &context,
               candidates,
               sizeof(candidates) / sizeof(candidates[0]),
               5000u,
               fake_scan_probe_candidate,
               &probe) == H2_H2LOADER_CLI_EXIT_RUNTIME);
    assert(probe.calls == 1u);
    assert(output.len == 0u);
}

static void test_parser_defaults_and_json_escaping(void) {
    const char *zero_delay[] = {
        "h2loader", "--post-delay", "0e0", "status",
    };
    const char *negative_delay[] = {
        "h2loader", "--post-delay", "-1", "status",
    };
    const char *download_timeout[] = {
        "h2loader", "--port", "fixture", "send-url", "--url",
        "http://example.test/update", "--bytes", "1", "--sha256",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "--download-timeout", "660",
    };
    fake_output_t output = {0};
    h2_command_io_api_t io = {.user = &output, .vtable = &output_vtable};
    h2_h2loader_cli_config_t config = {
        .stdout_io = &io,
        .stderr_io = &io,
    };
    h2_h2loader_cli_context_t context = {.config = &config};

    assert(run_cli(&output, 4, zero_delay) == H2_H2LOADER_CLI_EXIT_USAGE);
    assert(output.len == 0u);
    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 4, negative_delay) == H2_H2LOADER_CLI_EXIT_USAGE);
    assert(strstr(output.bytes, "usage: h2loader") != NULL);
    memset(&output, 0, sizeof(output));
    assert(run_cli(&output, 12, download_timeout) == H2_H2LOADER_CLI_EXIT_RUNTIME);
    assert(strstr(output.bytes, "usage: h2loader") == NULL);

    memset(&output, 0, sizeof(output));
    assert(h2_h2loader_cli_output_json_string(
        &context, H2_H2LOADER_CLI_STREAM_STDOUT,
        "quote=\" slash=\\ line=\n") == H2_PAL_OK);
    assert(strcmp(output.bytes, "\"quote=\\\" slash=\\\\ line=\\u000a\"") == 0);
}

static void test_send_progress_reports_acknowledged_delivery(void) {
    fake_output_t output = {0};
    fake_time_t time = {.now_ms = 2000u};
    h2_command_io_api_t io = {.user = &output, .vtable = &output_vtable};
    h2_pal_time_api_t time_api = {.user = &time, .vtable = &time_vtable};
    h2_runtime_t runtime = {.time = &time_api};
    h2_h2loader_cli_config_t config = {
        .stdout_io = &io,
        .stderr_io = &io,
    };
    h2_h2loader_cli_context_t context = {
        .runtime = &runtime,
        .config = &config,
    };
    h2_h2loader_cli_send_progress_t progress = {
        .context = &context,
        .start_ms = 1000u,
        .last_report_ms = 1000u,
    };

    h2_h2loader_cli_send_progress(&progress, 50u, 100u);
    assert(strstr(
        output.bytes,
        "state=progress acked=50 total=100 percent=50.0 rate_bps=50") != NULL);
    size_t first_len = output.len;
    h2_h2loader_cli_send_progress(&progress, 52u, 100u);
    assert(output.len == first_len);
    h2_h2loader_cli_send_progress(&progress, 100u, 100u);
    assert(strstr(
        output.bytes,
        "state=progress acked=100 total=100 percent=100.0 rate_bps=100") != NULL);
}

static void test_send_reports_unreadable_file(void) {
    const char *argv[] = {
        "h2loader", "--port", "/dev/null", "send", "--file",
        "bazel-bin/x.update.tar.zlib",
    };
    fake_output_t output = {0};

    assert(run_cli(&output, 6, argv) == H2_H2LOADER_CLI_EXIT_RUNTIME);
    assert(strstr(output.bytes,
        "h2loader: send failed step=stat file=bazel-bin/x.update.tar.zlib") != NULL);
    assert(strstr(output.bytes, "PAL filesystem mount") != NULL);
}

int main(void) {
    test_help_and_usage();
    test_ble_transport_routes_the_shared_device_command();
    test_legacy_upgrade_is_rejected_before_connect();
    test_check_reports_runtime_capabilities();
    test_ble_acquired_only_when_requested();
    test_bleikcp_speed_ble_lifecycle_boundaries();
    test_scan_reports_output_failures();
    test_scan_binds_live_status_to_exact_serial_port();
    test_parser_defaults_and_json_escaping();
    test_send_progress_reports_acknowledged_delivery();
    test_send_reports_unreadable_file();
    puts("h2loader cli app tests passed");
    return 0;
}
