#include "h2_h2loader_host.h"
#include "h2_h2loader_host_factory.h"
#include "h2_h2loader_host_flash.h"
#include "h2_h2loader_host_internal.h"
#include "h2_h2loader_host_scheduler.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *test_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static const h2_pal_mem_api_t test_mem = {
    .user = NULL,
    .vtable = &test_mem_vtable,
};

static h2_h2loader_host_status_t command_status(
    h2_h2loader_host_active_role_t role,
    uint32_t command_availability) {
    h2_h2loader_host_status_t status = {0};
    status.active_role = role;
    status.boot_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
    status.mfg_mode = 1u;
    status.command_availability = command_availability;
    return status;
}

static void test_typed_command_role_parity(void) {
    h2_h2loader_host_status_t app = command_status(
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP,
        H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATUS |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_APP |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_UPGRADE |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_ABORT |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_SCAN |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_DUMP);
    h2_h2loader_host_status_t loader = command_status(
        H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER,
        H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATUS |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_APP |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_ABORT |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_SCAN |
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_UPGRADE);
    assert(h2_h2loader_host_command_validate(
               &app,
               0u,
               H2_H2LOADER_HOST_COMMAND_STATUS) == H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &loader,
               0u,
               H2_H2LOADER_HOST_COMMAND_STATUS) == H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &app,
               0u,
               H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER) == H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &loader, 0u, H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER) ==
           H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &loader,
               0u,
               H2_H2LOADER_HOST_COMMAND_REBOOT_APP) == H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &app, 0u, H2_H2LOADER_HOST_COMMAND_REBOOT_APP) ==
           H2_PAL_OK);
    loader.command_availability =
        H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER;
    assert(h2_h2loader_host_command_validate(
               &loader,
               0u,
               H2_H2LOADER_HOST_COMMAND_REBOOT_APP) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_h2loader_host_command_validate(
               &loader,
               0u,
               H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER) == H2_PAL_OK);
    loader.command_availability =
        H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_APP;
    assert(h2_h2loader_host_command_validate(
               &loader,
               0u,
               H2_H2LOADER_HOST_COMMAND_REBOOT_APP) == H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &loader,
               0u,
               H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER) ==
           H2_PAL_ERR_INVALID_STATE);
    loader.command_availability =
        H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_ABORT |
        H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_UPGRADE |
        H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_SCAN;
    assert(h2_h2loader_host_command_validate(
               &loader, 0u, H2_H2LOADER_HOST_COMMAND_STAGE_ABORT) == H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &app, 0u, H2_H2LOADER_HOST_COMMAND_STAGE_ABORT) ==
           H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &app, 0u, H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE) == H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &loader, 0u, H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE) == H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &loader, 1u, H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_h2loader_host_command_validate(
               &loader, 0u, H2_H2LOADER_HOST_COMMAND_WIFI_SCAN) == H2_PAL_OK);
    loader.command_availability &=
        ~H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_DISCONNECT;
    loader.capabilities |= H2_H2LOADER_HOST_CAPABILITY_WIFI;
    assert(h2_h2loader_host_command_validate(
               &loader, 0u, H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT) ==
           H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &app, 0u, H2_H2LOADER_HOST_COMMAND_WIFI_SCAN) ==
           H2_PAL_OK);
    assert(h2_h2loader_host_command_validate(
               &app,
               1u,
               H2_H2LOADER_HOST_COMMAND_REBOOT_APP) ==
           H2_PAL_ERR_INVALID_STATE);
    app.command_availability &=
        ~H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_DUMP;
    assert(h2_h2loader_host_command_validate(
               &app,
               0u,
               H2_H2LOADER_HOST_COMMAND_COREDUMP_DUMP) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_h2loader_host_command_validate(
               &app,
               0u,
               (h2_h2loader_host_command_t)99) == H2_PAL_ERR_INVALID_ARG);
}

static void test_typed_command_wire_contract(void) {
    h2_h2loader_host_status_t app = command_status(
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP,
        H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL);
    h2_h2loader_host_command_request_t request = {
        .command = H2_H2LOADER_HOST_COMMAND_STATUS,
        .status = &app,
    };
    h2_h2loader_host_command_contract_t contract = {0};
    assert(h2_h2loader_host_command_contract(
               &request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.line, "h2loader status\n") == 0);
    assert(strcmp(contract.marker, "H2_LOADER_STATUS ") == 0);
    assert(contract.success_token == NULL);
    assert(contract.marker_is_success == 1u);
    assert(contract.lifecycle_transition == 0u);

    request.command = H2_H2LOADER_HOST_COMMAND_STATS;
    assert(h2_h2loader_host_command_contract(
               &request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.line, "h2loader stats\n") == 0);
    assert(strcmp(contract.marker, "H2_LOADER_STATUS ") == 0);
    assert(contract.success_token == NULL);
    assert(contract.marker_is_success == 1u);

    request.command = H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER;
    assert(h2_h2loader_host_command_contract(
               &request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.line, "h2loader reboot loader\n") == 0);
    assert(strcmp(contract.marker, "H2_LOADER_REBOOT ") == 0);
    assert(strcmp(contract.success_token, "result=accepted") == 0);
    assert(strcmp(contract.accepted_disconnect_token,
                  "H2_LOADER_REBOOT target=loader result=accepted") == 0);
    assert(contract.lifecycle_transition == 1u);

    request.command = H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE;
    assert(h2_h2loader_host_command_contract(
               &request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.line, "h2loader reboot upgrade\n") == 0);
    assert(strcmp(contract.marker, "H2_LOADER_REBOOT ") == 0);
    assert(strcmp(contract.success_token, "result=accepted") == 0);
    assert(strcmp(
               contract.accepted_disconnect_token,
               "H2_LOADER_REBOOT target=upgrade result=accepted") == 0);

    h2_h2loader_host_status_t loader = command_status(
        H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER,
        H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL);
    request.command = H2_H2LOADER_HOST_COMMAND_STATUS;
    request.status = &loader;
    assert(h2_h2loader_host_command_contract(
               &request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.marker, "H2_LOADER_STATUS ") == 0);
    assert(contract.marker_is_success == 1u);

    request.command = H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER;
    assert(h2_h2loader_host_command_contract(
               &request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.line, "h2loader reboot loader\n") == 0);
    assert(strcmp(contract.marker, "H2_LOADER_REBOOT ") == 0);
    assert(strcmp(contract.success_token, "result=accepted") == 0);
    assert(strcmp(
               contract.accepted_disconnect_token,
               "H2_LOADER_REBOOT target=loader result=accepted") == 0);
    assert(contract.lifecycle_transition == 1u);

    request.command = H2_H2LOADER_HOST_COMMAND_WIFI_SCAN;
    request.wifi_scan_limit = 0u;
    request.wifi_scan_timeout_ms = 0u;
    assert(h2_h2loader_host_command_contract(&request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.line,
        "h2loader wifi scan --limit 16 --timeout-ms 10000\n") == 0);
    assert(strcmp(contract.marker, "H2_LOADER_WIFI_SCAN_DONE ") == 0);
    assert(strcmp(contract.success_token, "result=OK") == 0);
    request.wifi_scan_limit = 3u;
    request.wifi_scan_timeout_ms = 2500u;
    assert(h2_h2loader_host_command_contract(&request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.line,
        "h2loader wifi scan --limit 3 --timeout-ms 2500\n") == 0);
    request.wifi_scan_limit = H2_H2LOADER_HOST_WIFI_SCAN_MAX_LIMIT + 1u;
    assert(h2_h2loader_host_command_contract(&request, &contract) ==
        H2_PAL_ERR_INVALID_ARG);
    request.wifi_scan_limit = 1u;
    request.wifi_scan_timeout_ms =
        H2_H2LOADER_HOST_WIFI_SCAN_MAX_TIMEOUT_MS + 1u;
    assert(h2_h2loader_host_command_contract(&request, &contract) ==
        H2_PAL_ERR_INVALID_ARG);

    request.command = H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT;
    request.ssid = "factory";
    request.password = "secret";
    assert(h2_h2loader_host_command_contract(&request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.line, "h2loader wifi connect factory secret\n") == 0);
    assert(strcmp(contract.success_token, "result=connecting") == 0);
    request.password = "bad password";
    assert(h2_h2loader_host_command_contract(&request, &contract) == H2_PAL_ERR_INVALID_ARG);

    request.command = H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT;
    request.ssid = NULL;
    request.password = NULL;
    assert(h2_h2loader_host_command_contract(&request, &contract) == H2_PAL_OK);
    assert(strcmp(contract.marker, "H2_LOADER_WIFI ") == 0);
    assert(strcmp(contract.success_token, "result=disconnected") == 0);
    request.command = H2_H2LOADER_HOST_COMMAND_STAGE_URL;
    request.url = "https://example.test/update.tar.zlib";
    request.expected_bytes = 42u;
    request.expected_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    assert(h2_h2loader_host_command_contract(&request, &contract) == H2_PAL_OK);
    assert(strstr(contract.line, "h2loader stage url https://example.test/") == contract.line);
    request.url = "https://bad url";
    assert(h2_h2loader_host_command_contract(&request, &contract) == H2_PAL_ERR_INVALID_ARG);
}

static void test_typed_command_terminal_contract(void) {
    const h2_h2loader_host_command_contract_t contract = {
        .line = "h2loader reboot app\n",
        .marker = "H2_LOADER_REBOOT ",
        .success_token = "target=app result=accepted",
    };
    const h2_h2loader_host_command_contract_t status_contract = {
        .line = "h2loader status\n",
        .marker = "H2_LOADER_STATUS ",
        .marker_is_success = 1u,
    };
    static const uint8_t status_ok[] =
        "noise\nH2_LOADER_STATUS board=amoled target=esp32s3 chip=esp32s3 "
        "capabilities=0x00000007 command_availability=0x000f3f3f "
        "boot_intent=auto active_role=app active_version=v1 "
        "active_checksum=abababababababababababababababababababababababababababababababab "
        "active_image_size=1 running_partition=2 next_partition=2 last_result=0\n";
    static const uint8_t status_error_only[] =
        "H2_LOADER_STATUS_ERROR code=-4\n";
    static const uint8_t status_unsupported[] =
        "H2_LOADER_STATUS result=unsupported\n";
    static const uint8_t ok[] =
        "noise\nH2_LOADER_REBOOT target=app result=accepted\n";
    static const uint8_t unsupported[] =
        "H2_LOADER_REBOOT result=unsupported\n";
    static const uint8_t unavailable[] =
        "H2_LOADER_REBOOT result=unavailable\n";
    static const uint8_t usage[] =
        "H2_LOADER_REBOOT usage: h2loader reboot app\n";
    static const uint8_t invalid[] =
        "H2_LOADER_REBOOT result=invalid_command\n";
    static const uint8_t error[] =
        "H2_LOADER_REBOOT result=error\n";
    static const uint8_t failed[] =
        "H2_LOADER_REBOOT result=fail\n";
    static const uint8_t marker_without_result[] =
        "H2_LOADER_REBOOT code=0\n";
    static const uint8_t malformed_success[] =
        "H2_LOADER_REBOOT target=app result=accepted-extra\n";
    static const uint8_t incomplete[] = "result=OK\n";
    static const uint8_t stale_error[] =
        "H2_LOADER_REBOOT result=error\n"
        "H2_LOADER_REBOOT target=app result=accepted\n";
    static const uint8_t unrelated_error[] =
        "diagnostic result=error\n"
        "H2_LOADER_REBOOT target=app result=accepted\n";

    assert(h2_h2loader_host_command_parse_terminal(
               ok, sizeof(ok) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);
    assert(h2_h2loader_host_command_parse_terminal(
               unsupported, sizeof(unsupported) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_UNSUPPORTED);
    assert(h2_h2loader_host_command_parse_terminal(
               unavailable, sizeof(unavailable) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_UNSUPPORTED);
    assert(h2_h2loader_host_command_parse_terminal(
               usage, sizeof(usage) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_USAGE);
    assert(h2_h2loader_host_command_parse_terminal(
               invalid, sizeof(invalid) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_USAGE);
    assert(h2_h2loader_host_command_parse_terminal(
               error, sizeof(error) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_ERROR);
    assert(h2_h2loader_host_command_parse_terminal(
               failed, sizeof(failed) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_ERROR);
    assert(h2_h2loader_host_command_parse_terminal(
               marker_without_result,
               sizeof(marker_without_result) - 1u,
               &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE);
    assert(h2_h2loader_host_command_parse_terminal(
               malformed_success,
               sizeof(malformed_success) - 1u,
               &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE);
    assert(h2_h2loader_host_command_parse_terminal(
               incomplete, sizeof(incomplete) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE);
    assert(h2_h2loader_host_command_parse_terminal(
               stale_error, sizeof(stale_error) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_ERROR);
    assert(h2_h2loader_host_command_parse_terminal(
               unrelated_error, sizeof(unrelated_error) - 1u, &contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);

    assert(h2_h2loader_host_command_parse_terminal(
               status_ok, sizeof(status_ok) - 1u, &status_contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);
    assert(h2_h2loader_host_command_parse_terminal(
               status_error_only,
               sizeof(status_error_only) - 1u,
               &status_contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE);
    assert(h2_h2loader_host_command_parse_terminal(
               status_unsupported,
               sizeof(status_unsupported) - 1u,
               &status_contract) ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_UNSUPPORTED);
}

typedef struct command_transport_fixture {
    const uint8_t *response;
    size_t response_len;
    h2_pal_result_t write_result;
    h2_pal_result_t read_result;
    h2_pal_result_t output_result;
    size_t write_count;
    size_t read_count;
    size_t output_count;
    size_t finish_count;
    size_t output_chunk_size;
    h2_pal_result_t finish_result;
    int cancelled;
    int cancel_after_output;
    char line[64];
    char marker[64];
    uint8_t output[128];
    size_t output_len;
} command_transport_fixture_t;

static h2_pal_result_t command_transport_finish(void *user) {
    command_transport_fixture_t *fixture = user;
    ++fixture->finish_count;
    return fixture->finish_result;
}

static h2_pal_result_t command_transport_write(
    void *user,
    const char *line) {
    command_transport_fixture_t *fixture = user;
    ++fixture->write_count;
    snprintf(fixture->line, sizeof(fixture->line), "%s", line);
    return fixture->write_result;
}

static h2_pal_result_t command_transport_read(
    void *user,
    const char *marker,
    uint8_t *response,
    size_t response_size,
    size_t *out_response_len,
    h2_h2loader_host_command_output_fn on_output,
    void *output_user) {
    command_transport_fixture_t *fixture = user;
    ++fixture->read_count;
    snprintf(fixture->marker, sizeof(fixture->marker), "%s", marker);
    size_t copied = fixture->response_len;
    if (copied > response_size) {
        copied = response_size;
    }
    memcpy(response, fixture->response, copied);
    *out_response_len = copied;
    if (copied > 0u && on_output != NULL) {
        size_t delivered = 0u;
        size_t chunk_size = fixture->output_chunk_size == 0u
            ? copied : fixture->output_chunk_size;
        while (delivered < copied) {
            size_t chunk = copied - delivered;
            if (chunk > chunk_size) chunk = chunk_size;
            h2_pal_result_t output_rc = on_output(
                output_user, &response[delivered], chunk);
            if (output_rc != H2_PAL_OK) return output_rc;
            delivered += chunk;
        }
    }
    return fixture->read_result;
}

static h2_pal_result_t command_transport_output(
    void *user,
    const uint8_t *data,
    size_t len) {
    command_transport_fixture_t *fixture = user;
    ++fixture->output_count;
    assert(len <= sizeof(fixture->output));
    memcpy(fixture->output, data, len);
    fixture->output_len = len;
    if (fixture->cancel_after_output) {
        fixture->cancelled = 1;
    }
    return fixture->output_result;
}

static int command_transport_cancelled(void *user) {
    return ((command_transport_fixture_t *)user)->cancelled;
}

static void test_typed_command_transport_execution(void) {
    static const uint8_t ok[] =
        "diagnostic\nH2_LOADER_STATUS board=amoled active_role=app\n";
    static const uint8_t unsupported[] =
        "H2_LOADER_STATUS result=unsupported\n";
    static const uint8_t error[] =
        "H2_LOADER_STATUS result=fail\n";
    static const uint8_t reboot_ok[] =
        "H2_LOADER_REBOOT target=loader result=accepted\n"
        "H2_LOADER_REBOOT_FINAL target=loader result=OK code=0\n";
    static const uint8_t reboot_failed[] =
        "H2_LOADER_REBOOT target=loader result=accepted\n"
        "H2_LOADER_REBOOT_FINAL target=loader result=fail code=-4\n";
    static const uint8_t reboot_disconnected[] =
        "H2_LOADER_REBOOT target=loader result=accepted\n";
    h2_h2loader_host_status_t app = command_status(
        H2_H2LOADER_HOST_ACTIVE_ROLE_APP,
        H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL);
    command_transport_fixture_t fixture = {
        .response = ok,
        .response_len = sizeof(ok) - 1u,
        .write_result = H2_PAL_OK,
        .read_result = H2_PAL_OK,
        .output_result = H2_PAL_OK,
        .finish_result = H2_PAL_OK,
    };
    h2_h2loader_host_command_request_t request = {
        .command = H2_H2LOADER_HOST_COMMAND_STATUS,
        .status = &app,
        .is_cancelled = command_transport_cancelled,
        .cancel_user = &fixture,
        .on_output = command_transport_output,
        .output_user = &fixture,
    };
    h2_h2loader_host_command_result_t result;

    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_OK);
    assert(fixture.write_count == 1u);
    assert(fixture.read_count == 1u);
    assert(fixture.output_count == 1u);
    assert(strcmp(fixture.line, "h2loader status\n") == 0);
    assert(strcmp(fixture.marker, "H2_LOADER_STATUS ") == 0);
    assert(fixture.output_len == sizeof(ok) - 1u);
    assert(memcmp(fixture.output, ok, sizeof(ok) - 1u) == 0);
    assert(result.transport_result == H2_PAL_OK);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);

    fixture.finish_count = 0u;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               command_transport_finish,
               &request,
               &result) == H2_PAL_OK);
    assert(fixture.finish_count == 1u);
    assert(result.transport_result == H2_PAL_OK);
    assert(result.output_bytes == sizeof(ok) - 1u);
    assert(result.output_truncated == 0u);

    fixture.response = unsupported;
    fixture.response_len = sizeof(unsupported) - 1u;
    fixture.output_count = 0u;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               command_transport_finish,
               &request,
               &result) == H2_PAL_ERR_UNSUPPORTED);
    assert(fixture.finish_count == 2u);
    assert(result.terminal ==
           H2_H2LOADER_HOST_COMMAND_TERMINAL_UNSUPPORTED);

    fixture.response = error;
    fixture.response_len = sizeof(error) - 1u;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               command_transport_finish,
               &request,
               &result) == H2_PAL_ERR_IO);
    assert(fixture.finish_count == 3u);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_ERROR);

    fixture.response = ok;
    fixture.response_len = sizeof(ok) - 1u;
    fixture.read_result = H2_PAL_ERR_NO_SPACE;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_ERR_NO_SPACE);
    assert(result.output_truncated == 1u);
    assert(result.output_bytes == sizeof(ok) - 1u);

    fixture.read_result = H2_PAL_OK;
    fixture.cancelled = 1;
    size_t prior_writes = fixture.write_count;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_EXIT);
    assert(fixture.write_count == prior_writes);

    fixture.cancelled = 0;
    fixture.cancel_after_output = 1;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_EXIT);
    assert(result.transport_result == H2_PAL_EXIT);

    fixture.cancelled = 0;
    fixture.cancel_after_output = 0;
    fixture.output_result = H2_PAL_ERR_IO;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_ERR_IO);
    assert(result.transport_result == H2_PAL_ERR_IO);

    h2_h2loader_host_status_t loader = command_status(
        H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER,
        H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL);
    request.command = H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER;
    request.status = &loader;
    fixture.output_result = H2_PAL_OK;
    fixture.response = reboot_ok;
    fixture.response_len = sizeof(reboot_ok) - 1u;
    fixture.read_result = H2_PAL_OK;
    fixture.finish_count = 0u;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_OK);
    assert(strcmp(fixture.marker, "H2_LOADER_REBOOT ") == 0);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);

    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               command_transport_finish,
               &request,
               &result) == H2_PAL_OK);
    assert(fixture.finish_count == 1u);
    assert(result.transport_result == H2_PAL_OK);

    fixture.finish_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               command_transport_finish,
               &request,
               &result) == H2_PAL_ERR_TIMEOUT);
    assert(fixture.finish_count == 2u);
    assert(result.transport_result == H2_PAL_ERR_TIMEOUT);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);
    fixture.finish_result = H2_PAL_OK;

    fixture.response = reboot_failed;
    fixture.response_len = sizeof(reboot_failed) - 1u;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_OK);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);

    fixture.response = reboot_disconnected;
    fixture.response_len = sizeof(reboot_disconnected) - 1u;
    fixture.read_result = H2_PAL_ERR_CLOSED;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_OK);
    assert(result.transport_result == H2_PAL_ERR_CLOSED);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);

    /* Browser transports surface the reboot as a read timeout rather than a
     * disconnect; the acknowledged marker must still resolve as accepted. */
    fixture.read_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_OK);
    assert(result.transport_result == H2_PAL_ERR_TIMEOUT);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);

    /* A timeout with no acknowledged marker in the buffer is a real failure. */
    static const uint8_t reboot_silent[] = "diagnostic noise\n";
    fixture.response = reboot_silent;
    fixture.response_len = sizeof(reboot_silent) - 1u;
    fixture.read_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_ERR_TIMEOUT);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE);

    static const uint8_t wifi_scan[] =
        "H2_LOADER_WIFI_SCAN_RESULT index=1 ssid_hex=666f6f\n"
        "H2_LOADER_WIFI_SCAN_DONE result=OK code=0 count=1\n";
    request.command = H2_H2LOADER_HOST_COMMAND_WIFI_SCAN;
    request.status = &loader;
    request.wifi_scan_limit = 1u;
    request.wifi_scan_timeout_ms = 2500u;
    fixture.response = wifi_scan;
    fixture.response_len = sizeof(wifi_scan) - 1u;
    fixture.read_result = H2_PAL_OK;
    fixture.output_count = 0u;
    fixture.output_chunk_size = 17u;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               NULL,
               &request,
               &result) == H2_PAL_OK);
    assert(fixture.output_count > 1u);
    assert(strcmp(fixture.line,
        "h2loader wifi scan --limit 1 --timeout-ms 2500\n") == 0);
    assert(strcmp(fixture.marker, "H2_LOADER_WIFI_SCAN_DONE ") == 0);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);

    fixture.finish_count = 0u;
    assert(h2_h2loader_host_command_execute_transport(
               &fixture,
               command_transport_write,
               command_transport_read,
               command_transport_finish,
               &request,
               &result) == H2_PAL_OK);
    assert(fixture.finish_count == 1u);
}

typedef struct resource_fixture {
    const uint8_t *bytes;
    size_t len;
} resource_fixture_t;

static h2_pal_result_t resource_read(
    void *user,
    const char *resource_name,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read) {
    resource_fixture_t *fixture = user;
    assert(strcmp(resource_name, "devkit-display.update.tar.zlib") == 0);
    *out_read = 0u;
    if (offset > fixture->len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t remaining = fixture->len - (size_t)offset;
    size_t take = remaining < out_size ? remaining : out_size;
    memcpy(out, &fixture->bytes[offset], take);
    *out_read = take;
    return H2_PAL_OK;
}

static void test_catalog(void) {
    static const uint8_t payload[] = "abc";
    static const char index[] =
        "{"
        "\"format\":1,"
        "\"version\":\"v1\","
        "\"firmware_count\":1,"
        "\"firmware\":[{"
        "\"platform\":\"esp\","
        "\"board\":\"devkit\","
        "\"target\":\"esp32s3\","
        "\"image\":\"display\","
        "\"role\":\"app\","
        "\"version\":\"v1\","
        "\"package_manifest\":{"
        "\"image_sha256\":"
        "\"0000000000000000000000000000000000000000000000000000000000000000\""
        "},"
        "\"assets\":[{"
        "\"name\":\"devkit-display.update.tar.zlib\","
        "\"operation\":\"managed-install\","
        "\"sha256\":"
        "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\","
        "\"size\":3"
        "}]"
        "}]}";
    resource_fixture_t resource = {
        .bytes = payload,
        .len = sizeof(payload) - 1u,
    };
    const h2_h2loader_host_catalog_config_t config = {
        .allocator = &test_mem,
        .index_json = (const uint8_t *)index,
        .index_json_len = sizeof(index) - 1u,
        .read_resource = resource_read,
        .resource_user = &resource,
    };
    h2_h2loader_host_catalog_t *catalog = NULL;
    assert(h2_h2loader_host_catalog_open(&config, &catalog) == H2_PAL_OK);
    size_t count = 0u;
    assert(h2_h2loader_host_catalog_count(catalog, &count) == H2_PAL_OK);
    assert(count == 1u);
    h2_h2loader_host_catalog_entry_t entry;
    assert(h2_h2loader_host_catalog_get(catalog, 0u, &entry) == H2_PAL_OK);
    assert(strcmp(entry.board, "devkit") == 0);
    assert(strcmp(entry.image, "display") == 0);
    assert(entry.bytes == 3u);
    size_t index_value = SIZE_MAX;
    count = 0u;
    assert(h2_h2loader_host_catalog_find(
               catalog,
               "devkit",
               "esp32s3",
               H2_H2LOADER_HOST_ASSET_ROLE_APP,
               H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL,
               &index_value,
               1u,
               &count) == H2_PAL_OK);
    assert(count == 1u && index_value == 0u);
    assert(h2_h2loader_host_catalog_close(&catalog) == H2_PAL_OK);
    assert(h2_h2loader_host_catalog_close(&catalog) == H2_PAL_OK);

    static const char missing_comma[] =
        "{\"format\":1 \"firmware\":[]}";
    h2_h2loader_host_catalog_config_t malformed_config = config;
    malformed_config.index_json = (const uint8_t *)missing_comma;
    malformed_config.index_json_len = sizeof(missing_comma) - 1u;
    assert(h2_h2loader_host_catalog_open(
               &malformed_config, &catalog) == H2_PAL_ERR_FORMAT);
    assert(catalog == NULL);
    static const char trailing_root[] =
        "{\"format\":1,\"firmware\":[]} false";
    malformed_config.index_json = (const uint8_t *)trailing_root;
    malformed_config.index_json_len = sizeof(trailing_root) - 1u;
    assert(h2_h2loader_host_catalog_open(
               &malformed_config, &catalog) == H2_PAL_ERR_FORMAT);
    assert(catalog == NULL);

    resource.len = 2u;
    assert(h2_h2loader_host_catalog_open(&config, &catalog) ==
           H2_PAL_ERR_TRUNCATED);
    assert(catalog == NULL);
}

static void test_status(void) {
    static const char checksum[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    h2_h2loader_host_status_t status;
    char v2_line[4096];
    int v2_len = snprintf(
        v2_line, sizeof(v2_line),
        "H2_LOADER_STATUS board=devkit target=esp32s3 chip=esp32s3 "
        "capabilities=0x00000005 command_availability=0x00000008 "
        "active_role=app active_version=v1 active_checksum=%s active_image_size=4096 "
        "running_partition=2 next_partition=2 boot_intent=auto "
        "stage_valid=0 stage_package_checksum=- stage_package_size=0 "
        "stage_image_checksum=- stage_image_size=0 stage_role=unknown "
        "stage_version=- stage_board=- stage_target=- "
        "partition_1_valid=1 partition_1_package_checksum=%s "
        "partition_1_package_size=3 partition_1_image_checksum=%s "
        "partition_1_image_size=2 partition_1_role=loader "
        "partition_1_version=v1 partition_1_board=devkit partition_1_target=esp32s3 "
        "partition_2_valid=1 partition_2_package_checksum=%s "
        "partition_2_package_size=3 partition_2_image_checksum=%s "
        "partition_2_image_size=2 partition_2_role=app "
        "partition_2_version=v1 partition_2_board=devkit partition_2_target=esp32s3 "
        "last_result=0 mfg_mode=1 mfg_steps=0000000000000000000000\n",
        checksum, checksum, checksum, checksum, checksum);
    assert(v2_len > 0 && (size_t)v2_len < sizeof(v2_line));
    assert(h2_h2loader_host_status_parse(v2_line, &status) == H2_PAL_OK);
    assert(status.active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
    assert(status.boot_intent == H2_H2LOADER_HOST_BOOT_INTENT_AUTO);
    assert(status.partition_1.role == H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER);
    assert(status.partition_2.valid == 1u);
    assert(strcmp(status.partition_2.package_checksum, checksum) == 0);
    assert(status.stage.valid == 0u);
    assert(strcmp(status.board, "devkit") == 0);
    assert(h2_h2loader_host_status_active_role(&status) ==
           H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
    assert(status.capabilities == 5u);
    assert(status.command_availability == UINT32_C(0x08));
    assert(status.running_partition == 2u);

    char noncanonical_hex[sizeof(v2_line)];
    strcpy(noncanonical_hex, v2_line);
    char *unsafe_version = strstr(noncanonical_hex, "active_version=v1");
    assert(unsafe_version != NULL);
    unsafe_version[strlen("active_version=v")] = '\t';
    assert(h2_h2loader_host_status_parse(noncanonical_hex, &status) ==
           H2_PAL_ERR_FORMAT);
    strcpy(noncanonical_hex, v2_line);
    char *package_size = strstr(noncanonical_hex, "stage_package_size=0");
    assert(package_size != NULL);
    package_size[strlen("stage_package_size=")] = 'x';
    assert(h2_h2loader_host_status_parse(noncanonical_hex, &status) ==
           H2_PAL_ERR_FORMAT);
    assert(h2_h2loader_host_status_parse(v2_line, &status) == H2_PAL_OK);

    h2_h2loader_host_catalog_entry_t asset = { 0 };
    strcpy(asset.board, "devkit");
    strcpy(asset.target, "esp32s3");
    strcpy(asset.image, "display");
    strcpy(asset.version, "v1");
    strcpy(asset.sha256, checksum);
    strcpy(asset.image_sha256, checksum);
    asset.role = H2_H2LOADER_HOST_ASSET_ROLE_APP;
    assert(h2_h2loader_host_status_verify_asset(&status, &asset) ==
           H2_PAL_OK);
    asset.identity_source =
        H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST;
    asset.image[0] = '\0';
    assert(h2_h2loader_host_status_verify_asset(&status, &asset) ==
           H2_PAL_OK);
    asset.identity_source = H2_H2LOADER_HOST_ASSET_IDENTITY_RELEASE_CATALOG;
    assert(h2_h2loader_host_status_verify_asset(&status, &asset) == H2_PAL_OK);
    memset(status.active_checksum, '0', H2_H2LOADER_HOST_SHA256_HEX_LEN);
    status.active_checksum[H2_H2LOADER_HOST_SHA256_HEX_LEN] = '\0';
    assert(h2_h2loader_host_status_verify_asset(&status, &asset) ==
           H2_PAL_ERR_INVALID_STATE);
    strcpy(status.active_checksum, checksum);
    status.stage.valid = 1u;
    assert(h2_h2loader_host_status_verify_asset(&status, &asset) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_h2loader_host_status_parse(
               "noise H2_LOADER_STATUS board=devkit\n",
               &status) == H2_PAL_ERR_FORMAT);
    assert(h2_h2loader_host_status_parse(
               "H2_APP_STATUS board=devkit\n",
               &status) == H2_PAL_ERR_FORMAT);

    assert(h2_h2loader_host_status_parse(
        "H2_LOADER_STATUS board=devkit target=esp32s3 chip=esp32s3 "
        "capabilities=0x00000005 command_availability=0x00000008 "
        "states=0x000000000001586a\n", &status) == H2_PAL_ERR_FORMAT);
}

static void write_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8u);
}

static void write_le32(uint8_t *out, uint32_t value) {
    for (size_t i = 0u; i < 4u; ++i) {
        out[i] = (uint8_t)(value >> (i * 8u));
    }
}

static void write_le64(uint8_t *out, uint64_t value) {
    for (size_t i = 0u; i < 8u; ++i) {
        out[i] = (uint8_t)(value >> (i * 8u));
    }
}

static h2_pal_result_t factory_read(
    void *user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read) {
    resource_fixture_t *fixture = user;
    *out_read = 0u;
    if (offset > fixture->len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t remaining = fixture->len - (size_t)offset;
    size_t take = remaining < out_size ? remaining : out_size;
    memcpy(out, &fixture->bytes[offset], take);
    *out_read = take;
    return H2_PAL_OK;
}

static void test_factory_bundle(void) {
    static const uint8_t member_sha[32] = {
        0x88u, 0xd4u, 0x26u, 0x6fu, 0xd4u, 0xe6u, 0x33u, 0x8du,
        0x13u, 0xb8u, 0x45u, 0xfcu, 0xf2u, 0x89u, 0x57u, 0x9du,
        0x20u, 0x9cu, 0x89u, 0x78u, 0x23u, 0xb9u, 0x21u, 0x7du,
        0xa3u, 0xe1u, 0x61u, 0x93u, 0x6fu, 0x03u, 0x15u, 0x89u,
    };
    uint8_t bundle[H2_H2LOADER_HOST_FACTORY_HEADER_SIZE + 4u] = { 0 };
    memcpy(bundle, "H2FB", 4u);
    write_le16(&bundle[4], 1u);
    write_le16(&bundle[6], H2_H2LOADER_HOST_FACTORY_DRIVER_ESP_ROM);
    write_le32(&bundle[8], 1u);
    write_le32(&bundle[12], 115200u);
    write_le32(&bundle[16], 1u);
    strcpy((char *)&bundle[20], "devkit");
    strcpy(
        (char *)&bundle[20 + H2_H2LOADER_HOST_IDENTITY_MAX_LEN],
        "esp32s3");
    const size_t record =
        20u + 2u * H2_H2LOADER_HOST_IDENTITY_MAX_LEN;
    write_le32(&bundle[record], 0x1000u);
    write_le64(
        &bundle[record + 4u],
        H2_H2LOADER_HOST_FACTORY_HEADER_SIZE);
    write_le64(&bundle[record + 12u], 4u);
    memcpy(&bundle[record + 20u], member_sha, sizeof(member_sha));
    strcpy((char *)&bundle[record + 52u], "loader.bin");
    memcpy(
        &bundle[H2_H2LOADER_HOST_FACTORY_HEADER_SIZE],
        "abcd",
        4u);
    resource_fixture_t fixture = {
        .bytes = bundle,
        .len = sizeof(bundle),
    };
    h2_h2loader_host_catalog_entry_t asset = {
        .bytes = sizeof(bundle),
        .role = H2_H2LOADER_HOST_ASSET_ROLE_LOADER,
        .operation = H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY,
    };
    strcpy(asset.board, "devkit");
    strcpy(asset.target, "esp32s3");
    h2_h2loader_host_factory_manifest_t manifest;
    assert(h2_h2loader_host_factory_open(
               &asset, factory_read, &fixture, &manifest) ==
           H2_PAL_OK);
    assert(manifest.file_count == 1u);
    assert(manifest.files[0].flash_offset == 0x1000u);
    uint8_t output[4];
    size_t read = 0u;
    assert(h2_h2loader_host_factory_read_member(
               &manifest,
               0u,
               factory_read,
               &fixture,
               0u,
               output,
               sizeof(output),
               &read) == H2_PAL_OK);
    assert(read == 4u && memcmp(output, "abcd", 4u) == 0);
    bundle[sizeof(bundle) - 1u] ^= 1u;
    assert(h2_h2loader_host_factory_open(
               &asset, factory_read, &fixture, &manifest) ==
           H2_PAL_ERR_FORMAT);
}

typedef struct flash_fixture {
    h2_pal_result_t prepare_result;
    int prepare_count;
    int erase_count;
    int write_count;
    int verify_count;
    int reset_count;
    int close_count;
} flash_fixture_t;

static h2_pal_result_t flash_prepare(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user) {
    (void)asset;
    (void)read_payload;
    (void)payload_user;
    flash_fixture_t *fixture = user;
    ++fixture->prepare_count;
    return fixture->prepare_result;
}

static h2_pal_result_t flash_erase(
    void *user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    (void)is_cancelled;
    (void)cancel_user;
    ++((flash_fixture_t *)user)->erase_count;
    return H2_PAL_OK;
}

static h2_pal_result_t flash_write(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    (void)asset;
    (void)read_payload;
    (void)payload_user;
    (void)is_cancelled;
    (void)cancel_user;
    (void)on_progress;
    (void)progress_user;
    ++((flash_fixture_t *)user)->write_count;
    return H2_PAL_OK;
}

static h2_pal_result_t flash_verify(
    void *user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    (void)is_cancelled;
    (void)cancel_user;
    ++((flash_fixture_t *)user)->verify_count;
    return H2_PAL_OK;
}

static h2_pal_result_t flash_reset(void *user) {
    ++((flash_fixture_t *)user)->reset_count;
    return H2_PAL_OK;
}

static h2_pal_result_t flash_close(void *user) {
    ++((flash_fixture_t *)user)->close_count;
    return H2_PAL_OK;
}

static h2_pal_result_t unused_payload_read(
    void *user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read) {
    (void)user;
    (void)offset;
    (void)out;
    (void)out_size;
    *out_read = 0u;
    return H2_PAL_OK;
}

static void test_recovery(void) {
    static const h2_h2loader_host_flash_driver_vtable_t vtable = {
        .prepare = flash_prepare,
        .erase = flash_erase,
        .write = flash_write,
        .verify = flash_verify,
        .reset_to_loader = flash_reset,
        .close = flash_close,
    };
    flash_fixture_t fixture = {
        .prepare_result = H2_PAL_OK,
    };
    h2_h2loader_host_flash_driver_t driver = {
        .user = &fixture,
        .vtable = &vtable,
    };
    h2_h2loader_host_catalog_entry_t asset = {
        .role = H2_H2LOADER_HOST_ASSET_ROLE_LOADER,
        .operation = H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY,
    };
    h2_h2loader_host_recovery_authorization_t authorization = {
        .transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL,
        .reason = H2_H2LOADER_HOST_RECOVERY_PROBE_FAILED,
        .probe_result = H2_PAL_ERR_TIMEOUT,
        .probe_completed_ms = 100u,
        .expires_ms = 200u,
        .probe_attempts = 2u,
        .identity_confirmed = 1u,
        .destructive_confirmed = 1u,
    };
    assert(h2_h2loader_host_recovery_run(
               &authorization,
               &asset,
               150u,
               &driver,
               unused_payload_read,
               NULL,
               NULL,
               NULL,
               NULL,
               NULL) == H2_PAL_OK);
    assert(fixture.erase_count == 1);
    assert(fixture.prepare_count == 1);
    assert(fixture.write_count == 1);
    assert(fixture.verify_count == 1);
    assert(fixture.reset_count == 1);
    assert(fixture.close_count == 1);
    fixture.prepare_result = H2_PAL_ERR_FORMAT;
    assert(h2_h2loader_host_recovery_run(
               &authorization,
               &asset,
               150u,
               &driver,
               unused_payload_read,
               NULL,
               NULL,
               NULL,
               NULL,
               NULL) == H2_PAL_ERR_FORMAT);
    assert(fixture.prepare_count == 2);
    assert(fixture.erase_count == 1);
    assert(fixture.close_count == 2);
    fixture.prepare_result = H2_PAL_OK;
    authorization.probe_result = H2_PAL_OK;
    assert(h2_h2loader_host_recovery_run(
               &authorization,
               &asset,
               150u,
               &driver,
               unused_payload_read,
               NULL,
               NULL,
               NULL,
               NULL,
               NULL) == H2_PAL_ERR_INVALID_STATE);
    assert(fixture.prepare_count == 2);
    assert(fixture.close_count == 3);
    assert(h2_h2loader_host_recovery_validate(
               &authorization, &asset, 150u) ==
           H2_PAL_ERR_INVALID_STATE);
    authorization.probe_result = H2_PAL_ERR_TIMEOUT;
    authorization.probe_attempts = 1u;
    assert(h2_h2loader_host_recovery_validate(
               &authorization, &asset, 150u) ==
           H2_PAL_ERR_INVALID_STATE);
    authorization.probe_attempts = 2u;
    authorization.transport = H2_H2LOADER_HOST_TRANSPORT_BLE;
    assert(h2_h2loader_host_recovery_validate(
               &authorization, &asset, 150u) ==
           H2_PAL_ERR_INVALID_STATE);
    authorization.transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL;
    authorization.reason = H2_H2LOADER_HOST_RECOVERY_BLANK_FIXTURE;
    authorization.probe_attempts = 1u;
    authorization.probe_result = H2_PAL_ERR_IO;
    assert(h2_h2loader_host_recovery_validate(
               &authorization, &asset, 150u) ==
           H2_PAL_ERR_INVALID_STATE);
    authorization.probe_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_h2loader_host_recovery_validate(
               &authorization, &asset, 150u) == H2_PAL_OK);
}

typedef struct operation_fixture {
    int connect_count;
    int stage_count;
    int activate_count;
    int disconnect_count;
    int rediscover_count;
    int read_status_count;
    int sleep_count;
    int cancelled;
    int bad_final_checksum;
    int stage_only;
    int fail_stage_status_once;
    h2_pal_result_t disconnect_result;
    h2_pal_result_t stage_result;
    h2_pal_result_t activate_result;
    h2_pal_result_t rediscover_result;
    h2_h2loader_host_operation_phase_t event_phase[32];
    h2_pal_result_t event_result[32];
    size_t event_count;
    char status_board[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    h2_h2loader_host_catalog_entry_t asset;
} operation_fixture_t;

static h2_pal_result_t operation_connect(
    void *user,
    h2_h2loader_host_status_t *out_status) {
    operation_fixture_t *fixture = user;
    ++fixture->connect_count;
    memset(out_status, 0, sizeof(*out_status));
    strcpy(out_status->board, fixture->status_board);
    strcpy(out_status->target, fixture->asset.target);
    out_status->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
    out_status->boot_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
    out_status->running_partition = 1u;
    out_status->next_partition = 1u;
    if (fixture->connect_count > 1) {
        if (fixture->stage_only) {
            out_status->stage.valid = 1u;
            out_status->stage.package_size = fixture->asset.bytes;
            strcpy(out_status->stage.package_checksum, fixture->asset.sha256);
            return H2_PAL_OK;
        }
        out_status->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
        out_status->running_partition = 2u;
        out_status->next_partition = 2u;
        strcpy(out_status->active_version, fixture->asset.version);
        strcpy(
            out_status->active_checksum,
            fixture->bad_final_checksum
                ? "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                : fixture->asset.image_sha256);
        out_status->partition_2.valid = 1u;
        out_status->partition_2.role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
        strcpy(out_status->partition_2.package_checksum, fixture->asset.sha256);
        strcpy(out_status->partition_2.image_checksum,
            fixture->bad_final_checksum
                ? "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                : fixture->asset.image_sha256);
        strcpy(out_status->partition_2.version, fixture->asset.version);
        strcpy(out_status->partition_2.board, fixture->asset.board);
        strcpy(out_status->partition_2.target, fixture->asset.target);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t operation_stage(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    (void)asset;
    (void)read_payload;
    (void)payload_user;
    (void)is_cancelled;
    (void)cancel_user;
    (void)on_progress;
    (void)progress_user;
    ++((operation_fixture_t *)user)->stage_count;
    return ((operation_fixture_t *)user)->stage_result;
}

static h2_pal_result_t operation_activate(
    void *user,
    const h2_h2loader_host_catalog_entry_t *asset) {
    (void)asset;
    ++((operation_fixture_t *)user)->activate_count;
    return ((operation_fixture_t *)user)->activate_result;
}

static h2_pal_result_t operation_disconnect(void *user) {
    operation_fixture_t *fixture = user;
    ++fixture->disconnect_count;
    return fixture->disconnect_result;
}

static h2_pal_result_t operation_read_status(
    void *user,
    h2_h2loader_host_status_t *out_status) {
    operation_fixture_t *fixture = user;
    ++fixture->read_status_count;
    if (fixture->fail_stage_status_once && fixture->read_status_count == 1) {
        return H2_PAL_ERR_TIMEOUT;
    }
    memset(out_status, 0, sizeof(*out_status));
    strcpy(out_status->board, fixture->asset.board);
    strcpy(out_status->target, fixture->asset.target);
    out_status->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
    out_status->stage.valid = 1u;
    out_status->stage.package_size = fixture->asset.bytes;
    strcpy(out_status->stage.package_checksum, fixture->asset.sha256);
    return H2_PAL_OK;
}

static h2_pal_result_t operation_rediscover(void *user) {
    ++((operation_fixture_t *)user)->rediscover_count;
    return ((operation_fixture_t *)user)->rediscover_result;
}

static h2_pal_result_t operation_sleep(void *user, uint32_t ms) {
    (void)ms;
    ++((operation_fixture_t *)user)->sleep_count;
    return H2_PAL_OK;
}

static int operation_cancelled(void *user) {
    return ((operation_fixture_t *)user)->cancelled;
}

static void operation_event(
    void *user,
    h2_h2loader_host_operation_phase_t phase,
    h2_pal_result_t result) {
    operation_fixture_t *fixture = user;
    assert(fixture->event_count <
           sizeof(fixture->event_phase) / sizeof(fixture->event_phase[0]));
    fixture->event_phase[fixture->event_count] = phase;
    fixture->event_result[fixture->event_count] = result;
    ++fixture->event_count;
}

static void test_managed_operation(void) {
    static const char package_sha[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char image_sha[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const h2_h2loader_host_managed_transport_vtable_t transport_vtable = {
        .connect = operation_connect,
        .stage = operation_stage,
        .activate = operation_activate,
        .read_status = operation_read_status,
        .disconnect = operation_disconnect,
        .rediscover = operation_rediscover,
    };
    static const h2_pal_time_vtable_t time_vtable = {
        .sleep_ms = operation_sleep,
    };
    operation_fixture_t fixture = { 0 };
    strcpy(fixture.status_board, "devkit");
    strcpy(fixture.asset.board, "devkit");
    strcpy(fixture.asset.target, "esp32s3");
    strcpy(fixture.asset.image, "display");
    strcpy(fixture.asset.version, "v1");
    strcpy(fixture.asset.sha256, package_sha);
    strcpy(fixture.asset.image_sha256, image_sha);
    fixture.asset.bytes = 3u;
    fixture.asset.role = H2_H2LOADER_HOST_ASSET_ROLE_APP;
    fixture.asset.operation =
        H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL;
    const h2_pal_time_api_t time = {
        .user = &fixture,
        .vtable = &time_vtable,
    };
    h2_h2loader_host_managed_operation_config_t config = {
        .time = &time,
        .transport = {
            .user = &fixture,
            .vtable = &transport_vtable,
        },
        .asset = &fixture.asset,
        .read_payload = unused_payload_read,
        .is_cancelled = operation_cancelled,
        .cancel_user = &fixture,
        .on_event = operation_event,
        .event_user = &fixture,
        .reconnect_delay_ms = 1u,
        .reconnect_attempts = 2u,
    };
    h2_h2loader_host_status_t final_status;
    assert(h2_h2loader_host_managed_operation_run(
               &config, &final_status) == H2_PAL_OK);
    assert(fixture.connect_count == 2);
    assert(fixture.stage_count == 1);
    assert(fixture.activate_count == 1);
    assert(fixture.disconnect_count == 2);
    assert(fixture.rediscover_count == 1);
    assert(fixture.sleep_count == 1);
    assert(final_status.active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP);
    assert(fixture.event_count > 0u);
    assert(fixture.event_phase[fixture.event_count - 1u] ==
           H2_H2LOADER_HOST_OPERATION_COMPLETE);
    assert(fixture.event_result[fixture.event_count - 1u] == H2_PAL_OK);

    fixture.stage_only = 1;
    fixture.connect_count = 0;
    fixture.stage_count = 0;
    fixture.activate_count = 0;
    fixture.disconnect_count = 0;
    fixture.rediscover_count = 0;
    fixture.read_status_count = 0;
    fixture.sleep_count = 0;
    assert(h2_h2loader_host_stage_operation_run(
               &config, &final_status) == H2_PAL_OK);
    assert(fixture.connect_count == 1);
    assert(fixture.stage_count == 1);
    assert(fixture.activate_count == 0);
    assert(fixture.disconnect_count == 1);
    assert(fixture.rediscover_count == 0);
    assert(fixture.read_status_count == 1);
    assert(strcmp(final_status.stage.package_checksum, package_sha) == 0);

    fixture.connect_count = 0;
    fixture.stage_count = 0;
    fixture.disconnect_count = 0;
    fixture.rediscover_count = 0;
    fixture.read_status_count = 0;
    fixture.sleep_count = 0;
    fixture.fail_stage_status_once = 1;
    assert(h2_h2loader_host_stage_operation_run(
               &config, &final_status) == H2_PAL_OK);
    assert(fixture.connect_count == 2);
    assert(fixture.stage_count == 1);
    assert(fixture.disconnect_count == 2);
    assert(fixture.rediscover_count == 1);
    assert(fixture.read_status_count == 1);
    assert(strcmp(final_status.stage.package_checksum, package_sha) == 0);
    fixture.fail_stage_status_once = 0;

    fixture.connect_count = 0;
    fixture.stage_count = 0;
    fixture.disconnect_count = 0;
    fixture.rediscover_count = 0;
    fixture.read_status_count = 0;
    fixture.sleep_count = 0;
    fixture.fail_stage_status_once = 1;
    fixture.disconnect_result = H2_PAL_ERR_IO;
    assert(h2_h2loader_host_stage_operation_run(
               &config, &final_status) == H2_PAL_ERR_IO);
    assert(fixture.connect_count == 1);
    assert(fixture.stage_count == 1);
    assert(fixture.disconnect_count == 1);
    assert(fixture.rediscover_count == 0);
    assert(fixture.read_status_count == 1);
    assert(fixture.sleep_count == 0);
    fixture.fail_stage_status_once = 0;
    fixture.disconnect_result = H2_PAL_OK;
    fixture.stage_only = 0;

    fixture.connect_count = 0;
    fixture.stage_count = 0;
    fixture.activate_count = 0;
    fixture.disconnect_count = 0;
    fixture.event_count = 0u;
    strcpy(fixture.asset.board, "wrong-board");
    assert(h2_h2loader_host_managed_operation_run(
               &config, &final_status) == H2_PAL_ERR_INVALID_STATE);
    assert(fixture.stage_count == 0);
    assert(fixture.disconnect_count == 1);
    assert(fixture.event_phase[fixture.event_count - 1u] ==
           H2_H2LOADER_HOST_OPERATION_PRECHECK);
    assert(fixture.event_result[fixture.event_count - 1u] ==
           H2_PAL_ERR_INVALID_STATE);

    strcpy(fixture.asset.board, "devkit");
    fixture.connect_count = 0;
    fixture.disconnect_count = 0;
    fixture.stage_count = 0;
    fixture.event_count = 0u;
    fixture.stage_result = H2_PAL_ERR_IO;
    assert(h2_h2loader_host_managed_operation_run(
               &config, &final_status) == H2_PAL_ERR_IO);
    assert(fixture.stage_count == 1);
    assert(fixture.disconnect_count == 1);
    assert(fixture.event_phase[fixture.event_count - 1u] ==
           H2_H2LOADER_HOST_OPERATION_STAGE);
    assert(fixture.event_result[fixture.event_count - 1u] ==
           H2_PAL_ERR_IO);

    fixture.stage_result = H2_PAL_OK;
    fixture.connect_count = 0;
    fixture.disconnect_count = 0;
    fixture.stage_count = 0;
    fixture.event_count = 0u;
    fixture.cancelled = 1;
    assert(h2_h2loader_host_managed_operation_run(
               &config, &final_status) == H2_PAL_EXIT);
    assert(fixture.connect_count == 0);
    assert(fixture.event_count == 1u);
    assert(fixture.event_phase[0] ==
           H2_H2LOADER_HOST_OPERATION_CONNECT);
    assert(fixture.event_result[0] == H2_PAL_EXIT);

    fixture.cancelled = 0;
    fixture.connect_count = 0;
    fixture.disconnect_count = 0;
    fixture.stage_count = 0;
    fixture.event_count = 0u;
    fixture.bad_final_checksum = 1;
    assert(h2_h2loader_host_managed_operation_run(
               &config, &final_status) == H2_PAL_ERR_INVALID_STATE);
    assert(fixture.connect_count == 3);
    assert(fixture.disconnect_count == 3);
    assert(fixture.event_phase[fixture.event_count - 1u] ==
           H2_H2LOADER_HOST_OPERATION_FINAL_VERIFY);
    assert(fixture.event_result[fixture.event_count - 1u] ==
           H2_PAL_ERR_INVALID_STATE);

    fixture.bad_final_checksum = 0;
    fixture.connect_count = 0;
    fixture.disconnect_count = 0;
    fixture.stage_count = 0;
    fixture.event_count = 0u;
    fixture.rediscover_result = H2_PAL_ERR_NOT_FOUND;
    assert(h2_h2loader_host_managed_operation_run(
               &config, &final_status) == H2_PAL_ERR_NOT_FOUND);
    assert(fixture.connect_count == 1);
    assert(fixture.rediscover_count >= 2);
    assert(fixture.event_phase[fixture.event_count - 1u] ==
           H2_H2LOADER_HOST_OPERATION_FINAL_VERIFY);
    assert(fixture.event_result[fixture.event_count - 1u] ==
           H2_PAL_ERR_NOT_FOUND);
}

typedef struct export_buffer {
    uint8_t bytes[16384];
    size_t len;
} export_buffer_t;

static h2_pal_result_t export_write(
    void *user,
    const uint8_t *bytes,
    size_t byte_count) {
    export_buffer_t *buffer = user;
    if (byte_count > sizeof(buffer->bytes) - buffer->len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(&buffer->bytes[buffer->len], bytes, byte_count);
    buffer->len += byte_count;
    return H2_PAL_OK;
}

static int buffer_contains(
    const uint8_t *bytes,
    size_t byte_count,
    const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0u || needle_len > byte_count) {
        return 0;
    }
    for (size_t i = 0u; i <= byte_count - needle_len; ++i) {
        if (memcmp(&bytes[i], needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_scheduler(void) {
    static const char sha[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    h2_h2loader_host_job_input_t inputs[3] = { 0 };
    for (size_t i = 0u; i < 3u; ++i) {
        (void)snprintf(
            inputs[i].fixture_slot,
            sizeof(inputs[i].fixture_slot),
            "slot-%u",
            (unsigned)i);
        inputs[i].candidate.transport =
            H2_H2LOADER_HOST_TRANSPORT_SERIAL;
        (void)snprintf(
            inputs[i].candidate.candidate_id,
            sizeof(inputs[i].candidate.candidate_id),
            "serial:fixture-%u",
            (unsigned)i);
        (void)snprintf(
            inputs[i].candidate.endpoint,
            sizeof(inputs[i].candidate.endpoint),
            "%cfixture-%u",
            i == 0u ? '=' : (i == 1u ? '+' : '-'),
            (unsigned)i);
        strcpy(inputs[i].asset.board, "devkit");
        strcpy(inputs[i].asset.target, "esp32s3");
        strcpy(inputs[i].asset.image, "display");
        strcpy(inputs[i].asset.version, "v1");
        strcpy(inputs[i].asset.sha256, sha);
        strcpy(inputs[i].asset.image_sha256, sha);
        inputs[i].asset.bytes = 4u;
        inputs[i].asset.role = H2_H2LOADER_HOST_ASSET_ROLE_APP;
        inputs[i].asset.operation =
            H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL;
    }
    const h2_h2loader_host_scheduler_config_t config = {
        .allocator = &test_mem,
        .jobs = inputs,
        .job_count = 3u,
        .max_concurrency = 2u,
    };
    h2_h2loader_host_scheduler_t *scheduler = NULL;
    char saved_candidate_id[H2_H2LOADER_HOST_CANDIDATE_ID_MAX_LEN];
    strcpy(saved_candidate_id, inputs[1].candidate.candidate_id);
    strcpy(
        inputs[1].candidate.candidate_id,
        inputs[0].candidate.candidate_id);
    assert(h2_h2loader_host_scheduler_open(
               &config, &scheduler) == H2_PAL_ERR_INVALID_ARG);
    assert(scheduler == NULL);
    strcpy(inputs[1].candidate.candidate_id, saved_candidate_id);
    inputs[0].asset.identity_source =
        H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST;
    inputs[0].asset.image[0] = '\0';
    assert(h2_h2loader_host_scheduler_open(&config, &scheduler) == H2_PAL_OK);
    assert(h2_h2loader_host_scheduler_close(&scheduler) == H2_PAL_OK);
    inputs[0].asset.identity_source =
        H2_H2LOADER_HOST_ASSET_IDENTITY_RELEASE_CATALOG;
    strcpy(inputs[0].asset.image, "display");
    for (size_t i = 0u; i < 2u; ++i) {
        inputs[i].candidate.usb_identity_valid = 1u;
        inputs[i].candidate.usb_vid = 0x303au;
        inputs[i].candidate.usb_pid = 0x1001u;
        strcpy(inputs[i].candidate.usb_serial, "same-device");
    }
    assert(h2_h2loader_host_scheduler_open(
               &config, &scheduler) == H2_PAL_ERR_INVALID_ARG);
    assert(scheduler == NULL);
    for (size_t i = 0u; i < 2u; ++i) {
        inputs[i].candidate.usb_identity_valid = 0u;
        inputs[i].candidate.usb_vid = 0u;
        inputs[i].candidate.usb_pid = 0u;
        inputs[i].candidate.usb_serial[0] = '\0';
    }
    assert(h2_h2loader_host_scheduler_open(
               &config, &scheduler) == H2_PAL_OK);
    size_t first = SIZE_MAX;
    size_t second = SIZE_MAX;
    size_t third = SIZE_MAX;
    h2_h2loader_host_job_input_t claimed;
    assert(h2_h2loader_host_scheduler_set_paused(
               NULL, 1) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_h2loader_host_scheduler_set_paused(
               scheduler, -1) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_h2loader_host_scheduler_set_paused(
               scheduler, 1) == H2_PAL_OK);
    assert(h2_h2loader_host_scheduler_claim(
               scheduler, 9u, &first, &claimed) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_h2loader_host_scheduler_set_paused(
               scheduler, 0) == H2_PAL_OK);
    assert(h2_h2loader_host_scheduler_claim(
               scheduler, 10u, &first, &claimed) == H2_PAL_OK);
    assert(strcmp(claimed.fixture_slot, "slot-0") == 0);
    assert(h2_h2loader_host_scheduler_claim(
               scheduler, 11u, &second, &claimed) == H2_PAL_OK);
    assert(h2_h2loader_host_scheduler_claim(
               scheduler, 12u, &third, &claimed) ==
           H2_PAL_ERR_WOULD_BLOCK);
    h2_h2loader_host_status_t status = { 0 };
    strcpy(status.board, "devkit");
    strcpy(status.target, "esp32s3");
    strcpy(status.active_version, "v1");
    strcpy(status.active_checksum, sha);
    status.active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
    status.boot_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
    memset(
        status.active_version,
        'x',
        sizeof(status.active_version));
    assert(h2_h2loader_host_scheduler_complete(
               scheduler,
               first,
               H2_PAL_OK,
               &status,
               NULL,
               20u) == H2_PAL_ERR_INVALID_ARG);
    strcpy(status.active_version, "v1");
    assert(h2_h2loader_host_scheduler_complete(
               scheduler,
               first,
               H2_PAL_OK,
               &status,
               NULL,
               20u) == H2_PAL_OK);
    assert(h2_h2loader_host_scheduler_claim(
               scheduler, 21u, &third, &claimed) == H2_PAL_OK);
    assert(strcmp(claimed.fixture_slot, "slot-2") == 0);
    assert(h2_h2loader_host_scheduler_complete(
               scheduler,
               second,
               H2_PAL_ERR_TIMEOUT,
               NULL,
               "@probe timed out, \"isolated\"",
               22u) == H2_PAL_OK);
    assert(h2_h2loader_host_scheduler_complete(
               scheduler,
               third,
               H2_PAL_OK,
               &status,
               NULL,
               23u) == H2_PAL_OK);
    export_buffer_t partial_json = { 0 };
    assert(h2_h2loader_host_scheduler_export_json(
               scheduler, export_write, &partial_json) == H2_PAL_OK);
    assert(buffer_contains(
        partial_json.bytes,
        partial_json.len,
        "\"state\":\"failed\""));
    assert(buffer_contains(
        partial_json.bytes,
        partial_json.len,
        "@probe timed out, \\\"isolated\\\""));
    export_buffer_t partial_csv = { 0 };
    assert(h2_h2loader_host_scheduler_export_csv(
               scheduler, export_write, &partial_csv) == H2_PAL_OK);
    assert(buffer_contains(
        partial_csv.bytes, partial_csv.len, "\"'=fixture-0\""));
    assert(buffer_contains(
        partial_csv.bytes, partial_csv.len, "\"'+fixture-1\""));
    assert(buffer_contains(
        partial_csv.bytes, partial_csv.len, "\"'-fixture-2\""));
    assert(buffer_contains(
        partial_csv.bytes,
        partial_csv.len,
        "\"'@probe timed out, \"\"isolated\"\"\""));
    assert(h2_h2loader_host_scheduler_retry(
               scheduler, second) == H2_PAL_OK);
    size_t retry = SIZE_MAX;
    assert(h2_h2loader_host_scheduler_claim(
               scheduler, 30u, &retry, &claimed) == H2_PAL_OK);
    assert(retry == second);
    assert(h2_h2loader_host_scheduler_complete(
               scheduler,
               retry,
               H2_PAL_OK,
               &status,
               NULL,
               31u) == H2_PAL_OK);
    h2_h2loader_host_job_result_t result;
    assert(h2_h2loader_host_scheduler_get(
               scheduler, second, &result) == H2_PAL_OK);
    assert(result.state == H2_H2LOADER_HOST_JOB_SUCCEEDED);
    assert(result.retry_count == 1u);

    export_buffer_t json = { 0 };
    assert(h2_h2loader_host_scheduler_export_json(
               scheduler, export_write, &json) == H2_PAL_OK);
    assert(json.len > 0u);
    assert(buffer_contains(
        json.bytes, json.len, "\"fixture_slot\":\"slot-0\""));
    assert(buffer_contains(
        json.bytes, json.len, "\"catalog_format\":1"));
    assert(buffer_contains(
        json.bytes, json.len, "\"operation\":\"managed\""));
    assert(buffer_contains(
        json.bytes, json.len, "\"final_boot_intent\":\"auto\""));
    export_buffer_t csv = { 0 };
    assert(h2_h2loader_host_scheduler_export_csv(
               scheduler, export_write, &csv) == H2_PAL_OK);
    assert(csv.len > 0u);
    assert(buffer_contains(csv.bytes, csv.len, "\"slot-2\""));
    assert(buffer_contains(csv.bytes, csv.len, "\"serial\""));
    assert(buffer_contains(csv.bytes, csv.len, "\"auto\""));
    assert(h2_h2loader_host_scheduler_close(&scheduler) == H2_PAL_OK);
    assert(scheduler == NULL);

    assert(h2_h2loader_host_scheduler_open(
               &config, &scheduler) == H2_PAL_OK);
    size_t cancelled = 0u;
    assert(h2_h2loader_host_scheduler_cancel_queued(
               scheduler, 40u, &cancelled) == H2_PAL_OK);
    assert(cancelled == 3u);
    assert(h2_h2loader_host_scheduler_get(
               scheduler, 1u, &result) == H2_PAL_OK);
    assert(result.state == H2_H2LOADER_HOST_JOB_CANCELLED);
    assert(result.result == H2_PAL_EXIT);
    assert(h2_h2loader_host_scheduler_retry(
               scheduler, 1u) == H2_PAL_OK);
    assert(h2_h2loader_host_scheduler_claim(
               scheduler, 41u, &retry, &claimed) == H2_PAL_OK);
    assert(retry == 1u);
    assert(h2_h2loader_host_scheduler_complete(
               scheduler,
               retry,
               H2_PAL_EXIT,
               NULL,
               "operator cancellation",
               42u) == H2_PAL_OK);
    assert(h2_h2loader_host_scheduler_get(
               scheduler, retry, &result) == H2_PAL_OK);
    assert(result.state == H2_H2LOADER_HOST_JOB_CANCELLED);
    assert(result.retry_count == 1u);
    assert(h2_h2loader_host_scheduler_close(&scheduler) == H2_PAL_OK);
}

typedef struct serial_control_fixture {
    h2_pal_result_t set_result;
    h2_pal_result_t stream_result;
    char events[4];
    size_t event_count;
    uint32_t line_mask;
    uint32_t asserted_lines;
} serial_control_fixture_t;

static h2_pal_result_t serial_control_open(
    void *user,
    const char *port_id,
    const h2_pal_uart_io_stream_config_t *config,
    h2_pal_serial_host_session_t **out_session) {
    serial_control_fixture_t *fixture = user;
    assert(strcmp(port_id, "control-port") == 0);
    assert(config->baud_rate == H2_H2LOADER_HOST_RELIABLE_SERIAL_BAUD);
    fixture->events[fixture->event_count++] = 'o';
    *out_session = (h2_pal_serial_host_session_t *)fixture;
    return H2_PAL_OK;
}

static h2_pal_result_t serial_control_set(
    void *user,
    h2_pal_serial_host_session_t *session,
    uint32_t line_mask,
    uint32_t asserted_lines) {
    serial_control_fixture_t *fixture = user;
    assert(session == (h2_pal_serial_host_session_t *)fixture);
    fixture->events[fixture->event_count++] = 's';
    fixture->line_mask = line_mask;
    fixture->asserted_lines = asserted_lines;
    return fixture->set_result;
}

static h2_pal_result_t serial_control_stream(
    void *user,
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_api_t **out_stream) {
    serial_control_fixture_t *fixture = user;
    assert(session == (h2_pal_serial_host_session_t *)fixture);
    fixture->events[fixture->event_count++] = 't';
    *out_stream = NULL;
    return fixture->stream_result;
}

static h2_pal_result_t serial_control_close(
    void *user,
    h2_pal_serial_host_session_t **inout_session) {
    serial_control_fixture_t *fixture = user;
    assert(*inout_session == (h2_pal_serial_host_session_t *)fixture);
    fixture->events[fixture->event_count++] = 'c';
    *inout_session = NULL;
    return H2_PAL_OK;
}

static const h2_pal_serial_host_vtable_t serial_control_vtable = {
    .open = serial_control_open,
    .session_stream = serial_control_stream,
    .set_control_lines = serial_control_set,
    .close = serial_control_close,
};

static void test_serial_deasserts_dtr_and_rts(void) {
    h2_pal_time_api_t time = {0};
    serial_control_fixture_t fixture = {
        .set_result = H2_PAL_OK,
        .stream_result = H2_PAL_ERR_IO,
    };
    h2_pal_serial_host_api_t serial = {
        .user = &fixture,
        .vtable = &serial_control_vtable,
    };
    h2_h2loader_host_serial_connection_config_t config = {
        .serial = &serial,
        .time = &time,
        .allocator = &test_mem,
        .port_id = "control-port",
    };
    h2_h2loader_host_serial_connection_t *connection = NULL;

    assert(h2_h2loader_host_serial_connect(&config, &connection) ==
        H2_PAL_ERR_IO);
    assert(connection == NULL);
    assert(fixture.event_count == 4u);
    assert(memcmp(fixture.events, "ostc", 4u) == 0);
    assert(fixture.line_mask ==
        (H2_PAL_SERIAL_HOST_CONTROL_DTR |
         H2_PAL_SERIAL_HOST_CONTROL_RTS));
    assert(fixture.asserted_lines == 0u);

    memset(&fixture, 0, sizeof(fixture));
    fixture.set_result = H2_PAL_ERR_UNSUPPORTED;
    fixture.stream_result = H2_PAL_ERR_IO;
    assert(h2_h2loader_host_serial_connect(&config, &connection) ==
        H2_PAL_ERR_IO);
    assert(fixture.event_count == 4u);
    assert(memcmp(fixture.events, "ostc", 4u) == 0);

    memset(&fixture, 0, sizeof(fixture));
    fixture.set_result = H2_PAL_ERR_TIMEOUT;
    fixture.stream_result = H2_PAL_ERR_IO;
    assert(h2_h2loader_host_serial_connect(&config, &connection) ==
        H2_PAL_ERR_TIMEOUT);
    assert(fixture.event_count == 3u);
    assert(memcmp(fixture.events, "osc", 3u) == 0);
}

int main(void) {
    test_typed_command_role_parity();
    test_typed_command_wire_contract();
    test_typed_command_terminal_contract();
    test_typed_command_transport_execution();
    test_catalog();
    test_status();
    test_factory_bundle();
    test_recovery();
    test_managed_operation();
    test_scheduler();
    test_serial_deasserts_dtr_and_rts();
    return 0;
}
