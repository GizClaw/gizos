#include "h2_h2loader_cli_internal.h"

#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <string.h>

typedef enum transport_scenario {
    TRANSPORT_SUCCESS = 0,
    TRANSPORT_TIMEOUT,
    TRANSPORT_REJECTED,
    TRANSPORT_READ_STATUS_FAILURE,
} transport_scenario_t;

typedef struct fake_transport {
    transport_scenario_t scenario;
    unsigned connect_count;
    unsigned status_count;
    unsigned execute_count;
    unsigned disconnect_count;
    uint64_t expected_bytes;
    char expected_sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
} fake_transport_t;

static h2_pal_result_t discard_write(
    void *user, const void *data, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
    (void)user;
    (void)data;
    (void)timeout_ms;
    *out_written = len;
    return H2_PAL_OK;
}

static h2_pal_result_t discard_flush(void *user) {
    (void)user;
    return H2_PAL_OK;
}

static const h2_command_io_vtable_t discard_vtable = {
    .write = discard_write,
    .flush = discard_flush,
};

static h2_pal_result_t fake_transport_connect(
    void *user,
    uint32_t command_timeout_ms,
    h2_h2loader_host_status_t *out_status) {
    fake_transport_t *transport = user;
    assert(command_timeout_ms == 660000u);
    ++transport->connect_count;
    ++transport->status_count;
    memset(out_status, 0, sizeof(*out_status));
    return H2_PAL_OK;
}

static h2_pal_result_t fake_transport_execute(
    void *user,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result) {
    fake_transport_t *transport = user;
    ++transport->execute_count;
    assert(request->command == H2_H2LOADER_HOST_COMMAND_STAGE_URL);
    transport->expected_bytes = request->expected_bytes;
    strcpy(transport->expected_sha256, request->expected_sha256);
    if (transport->scenario == TRANSPORT_TIMEOUT) return H2_PAL_ERR_TIMEOUT;
    out_result->terminal = transport->scenario == TRANSPORT_REJECTED
        ? H2_H2LOADER_HOST_COMMAND_TERMINAL_ERROR
        : H2_H2LOADER_HOST_COMMAND_TERMINAL_OK;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_transport_disconnect(void *user) {
    fake_transport_t *transport = user;
    ++transport->disconnect_count;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_transport_read_status(
    void *user,
    h2_h2loader_host_status_t *out_status) {
    fake_transport_t *transport = user;
    ++transport->status_count;
    if (transport->scenario == TRANSPORT_READ_STATUS_FAILURE) {
        return H2_PAL_ERR_IO;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->staged_valid = 1u;
    out_status->staged_bytes = transport->expected_bytes;
    strcpy(out_status->staged_checksum, transport->expected_sha256);
    return H2_PAL_OK;
}

static const h2_h2loader_cli_server_transport_vtable_t fake_transport_vtable = {
    .connect = fake_transport_connect,
    .execute = fake_transport_execute,
    .read_status = fake_transport_read_status,
    .disconnect = fake_transport_disconnect,
};

static void test_parse_route_and_timeout(void) {
    static const char sha[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *argv[] = {
        "--url", "https://example.test/pkg", "--bytes", "17", "--sha256", sha,
        "--host", "127.0.0.1", "--url-host", "device.test", "--url-path", "/exact",
        "--http-port", "8123", "--download-timeout", "12.5",
    };
    h2_h2loader_cli_server_options_t options;
    assert(h2_h2loader_cli_server_options_parse(16, argv, &options));
    assert(strcmp(options.bind_host, "127.0.0.1") == 0);
    assert(strcmp(options.url_host, "device.test") == 0);
    assert(strcmp(options.url_path, "/exact") == 0);
    assert(options.port == 8123u);
    assert(options.download_timeout_ms == 12500u);
}

static void test_file_mode_is_explicitly_unsupported(void) {
    h2_command_io_api_t io = {.vtable = &discard_vtable};
    h2_runtime_t runtime = {.mem = h2_pal_unsupported_mem_api()};
    h2_h2loader_cli_config_t config = {
        .stdout_io = &io,
        .stderr_io = &io,
    };
    h2_h2loader_cli_context_t context = {.runtime = &runtime, .config = &config};
    h2_h2loader_cli_options_t options = {.port = "fixture"};
    const char *argv[] = {"--file", "/tmp/fixture.pkg"};
    assert(h2_h2loader_cli_server_command_with_transport(
        &context, &options, 2, argv,
        &(h2_h2loader_cli_server_transport_api_t){
            .vtable = &fake_transport_vtable,
        }) == H2_H2LOADER_CLI_EXIT_RUNTIME);
}

static void run_terminal_scenario(
    transport_scenario_t scenario,
    int expected_exit,
    unsigned expected_connects,
    unsigned expected_statuses) {
    static const char sha[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    h2_command_io_api_t io = {.vtable = &discard_vtable};
    fake_transport_t transport_state = {.scenario = scenario};
    h2_runtime_t runtime = {
        .mem = h2_pal_unsupported_mem_api(),
        .time = h2_pal_unsupported_time_api(),
    };
    h2_h2loader_cli_config_t config = {
        .serial = h2_pal_unsupported_serial_host_api(),
        .stdout_io = &io,
        .stderr_io = &io,
    };
    h2_h2loader_cli_context_t context = {.runtime = &runtime, .config = &config};
    h2_h2loader_cli_options_t options = {.port = "fixture"};
    h2_h2loader_cli_server_transport_api_t transport = {
        .user = &transport_state,
        .vtable = &fake_transport_vtable,
    };
    const char *argv[] = {
        "--url", "https://example.test/pkg", "--bytes", "17", "--sha256", sha,
    };
    assert(h2_h2loader_cli_server_command_with_transport(
        &context, &options, 6, argv, &transport) == expected_exit);
    assert(transport_state.connect_count == expected_connects);
    assert(transport_state.status_count == expected_statuses);
    assert(transport_state.execute_count == 1u);
    assert(transport_state.disconnect_count == 1u);
}

static void test_url_terminal_paths(void) {
    run_terminal_scenario(
        TRANSPORT_TIMEOUT, H2_H2LOADER_CLI_EXIT_RUNTIME, 1u, 1u);
    run_terminal_scenario(
        TRANSPORT_REJECTED, H2_H2LOADER_CLI_EXIT_RUNTIME, 1u, 1u);
    run_terminal_scenario(
        TRANSPORT_READ_STATUS_FAILURE,
        H2_H2LOADER_CLI_EXIT_RUNTIME, 1u, 2u);
    run_terminal_scenario(
        TRANSPORT_SUCCESS, H2_H2LOADER_CLI_EXIT_OK, 1u, 2u);
}

int main(void) {
    test_parse_route_and_timeout();
    test_file_mode_is_explicitly_unsupported();
    test_url_terminal_paths();
    return 0;
}
