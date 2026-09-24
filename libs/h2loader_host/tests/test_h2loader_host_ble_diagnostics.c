#include "h2_h2loader_host.h"
#include "h2_bleikcp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
    int fail_mtu;
    int board_mismatch;
    unsigned opens;
    unsigned closes;
    unsigned writes;
    size_t response_offset;
    unsigned allocations;
    unsigned frees;
    unsigned disconnects;
    unsigned logs;
    h2_pal_result_t sink_result;
    char line[96];
} fixture_t;

static void *allocate(void *user, size_t size) {
    fixture_t *f = user;
    ++f->allocations;
    return malloc(size);
}

static void release(void *user, void *ptr) {
    fixture_t *f = user;
    ++f->frees;
    free(ptr);
}

static h2_pal_result_t connect_ble(
    void *user, const h2_pal_ble_addr_t *address,
    const h2_pal_ble_connect_params_t *params, uint16_t *out_handle) {
    fixture_t *f = user;
    (void)address;
    assert(params->timeout_ms != 0u);
    if (!f->fail_mtu && !f->board_mismatch) return H2_PAL_ERR_TIMEOUT;
    *out_handle = 7u;
    return H2_PAL_OK;
}

static h2_pal_result_t exchange_mtu(
    void *user, uint16_t handle, uint16_t *out_mtu, uint32_t timeout) {
    fixture_t *f = user;
    if (f->board_mismatch) {
        *out_mtu = 247u;
        return H2_PAL_OK;
    }
    assert(handle == 7u && timeout != 0u);
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t disconnect_ble(void *user, uint16_t handle) {
    fixture_t *f = user;
    assert(handle == 7u);
    ++f->disconnects;
    return H2_PAL_OK;
}

static h2_pal_result_t capture(void *user, const uint8_t *data, size_t len) {
    fixture_t *f = user;
    assert(len > 0u && len < sizeof(f->line));
    assert(data[len - 1u] == '\n');
    assert(memchr(data, '\0', len) == NULL);
    memcpy(f->line, data, len);
    f->line[len] = '\0';
    ++f->logs;
    return f->sink_result;
}

/* Fake the stream boundary while exercising the real Host status parser. */
bool h2_bleikcp_global_ready(void) { return true; }

int h2_bleikcp_client_open(
    const h2_bleikcp_api_t *api, const h2_bleikcp_config_t *config,
    uint16_t handle, uint16_t mtu, h2_bleikcp_t **out_stream) {
    fixture_t *f = api->ble->user;
    (void)config;
    assert(f->board_mismatch && handle == 7u && mtu == 247u);
    ++f->opens;
    *out_stream = (h2_bleikcp_t *)f;
    return H2_PAL_OK;
}

int h2_bleikcp_close(h2_bleikcp_t *stream) {
    fixture_t *f = (fixture_t *)stream;
    ++f->closes;
    return H2_PAL_OK;
}

int h2_bleikcp_write(
    h2_bleikcp_t *stream, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    fixture_t *f = (fixture_t *)stream;
    static const char command[] = "h2loader status\n";
    assert(timeout_ms != 0u && len == sizeof(command) - 1u);
    assert(memcmp(data, command, len) == 0);
    ++f->writes;
    return H2_PAL_OK;
}

int h2_bleikcp_flush(h2_bleikcp_t *stream, uint32_t timeout_ms) {
    (void)stream;
    assert(timeout_ms != 0u);
    return H2_PAL_OK;
}

int h2_bleikcp_read(
    h2_bleikcp_t *stream, uint8_t *out, size_t out_size,
    size_t *out_len, uint32_t timeout_ms) {
    fixture_t *f = (fixture_t *)stream;
    static const char response[] =
        "H2_LOADER_STATUS board=devkit target=esp32s3 chip=esp32s3 "
        "device_uid=102030405060 "
        "capabilities=0x00000005 command_availability=0x00000008 "
        "active_role=app active_version=v1 active_checksum=abababababababababababababababababababababababababababababababab active_image_size=4096 "
        "running_partition=2 next_partition=2 boot_intent=auto "
        "stage_valid=0 stage_package_checksum=- stage_package_size=0 "
        "stage_image_checksum=- stage_image_size=0 stage_role=unknown "
        "stage_version=- stage_board=- stage_target=- "
        "partition_1_valid=1 partition_1_package_checksum=abababababababababababababababababababababababababababababababab "
        "partition_1_package_size=3 partition_1_image_checksum=abababababababababababababababababababababababababababababababab "
        "partition_1_image_size=2 partition_1_role=loader "
        "partition_1_version=v1 partition_1_board=devkit partition_1_target=esp32s3 "
        "partition_2_valid=1 partition_2_package_checksum=abababababababababababababababababababababababababababababababab "
        "partition_2_package_size=3 partition_2_image_checksum=abababababababababababababababababababababababababababababababab "
        "partition_2_image_size=2 partition_2_role=app "
        "partition_2_version=v1 partition_2_board=devkit partition_2_target=esp32s3 "
        "last_result=0 mfg_mode=1 mfg_steps=0000000000000000000000\n";
    assert(timeout_ms != 0u && f->response_offset < sizeof(response) - 1u);
    size_t len = sizeof(response) - 1u - f->response_offset;
    if (len > out_size) len = out_size;
    memcpy(out, response + f->response_offset, len);
    f->response_offset += len;
    *out_len = len;
    return H2_PAL_OK;
}

static h2_pal_result_t monotonic_ms(void *user, uint64_t *out_ms) {
    (void)user;
    *out_ms = 1u;
    return H2_PAL_OK;
}

static void check_failure(
    int fail_mtu, int with_sink, int sink_fails, int board_mismatch) {
    fixture_t f = {0};
    f.fail_mtu = fail_mtu;
    f.board_mismatch = board_mismatch;
    f.sink_result = sink_fails ? H2_PAL_ERR_NO_SPACE : H2_PAL_OK;
    const h2_pal_mem_vtable_t memory_vtable = {
        .alloc = allocate, .free = release,
    };
    const h2_pal_mem_api_t memory = {.user = &f, .vtable = &memory_vtable};
    const h2_pal_ble_vtable_t ble_vtable = {
        .connect = connect_ble, .exchange_mtu = exchange_mtu,
        .disconnect = disconnect_ble,
    };
    const h2_pal_ble_api_t ble = {.user = &f, .vtable = &ble_vtable};
    /* The fake stream does not use task, sync or system event services.
     * Only the Host status read needs the monotonic clock. */
    const h2_pal_task_api_t task = {0};
    const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = monotonic_ms};
    const h2_pal_time_api_t time = {.vtable = &time_vtable};
    const h2_pal_sync_api_t sync = {0};
    const h2_pal_system_event_api_t event = {0};
    const h2_h2loader_host_ble_connection_config_t config = {
        .ble = &ble, .task = &task, .time = &time, .sync = &sync,
        .system_event = &event, .allocator = &memory,
        .advertised_board = board_mismatch ? "other-board" : NULL,
        .address = {.type = H2_PAL_BLE_ADDR_TYPE_PUBLIC},
        .on_log = with_sink ? capture : NULL, .log_user = &f,
    };
    h2_h2loader_host_ble_connection_t *connection = NULL;
    h2_h2loader_host_status_t status;
    const h2_pal_result_t expected = board_mismatch
        ? H2_PAL_ERR_INVALID_STATE
        : fail_mtu ? H2_PAL_ERR_IO : H2_PAL_ERR_TIMEOUT;
    assert(h2_h2loader_host_ble_connect(&config, &connection, &status) == expected);
    assert(connection == NULL);
    assert(f.allocations == 1u && f.frees == 1u);
    assert(f.disconnects == (unsigned)(fail_mtu || board_mismatch));
    assert(f.opens == (unsigned)board_mismatch);
    assert(f.closes == f.opens && f.writes == f.opens);
    if (board_mismatch) {
        assert(f.response_offset > 0u);
        assert(strcmp(status.board, "devkit") == 0);
    }
    assert(f.logs == (unsigned)with_sink);
    if (with_sink) {
        char expected_line[96];
        (void)snprintf(expected_line, sizeof(expected_line),
                       "H2_BLE_HOST_DIAG stage=%s rc=%d\n",
                       board_mismatch ? "status" : fail_mtu ? "mtu" : "connect", expected);
        assert(strcmp(f.line, expected_line) == 0);
    }
}

int main(void) {
    for (int stage = 0; stage != 2; ++stage) {
        check_failure(stage, 0, 0, 0);
        check_failure(stage, 1, 0, 0);
        check_failure(stage, 1, 1, 0);
    }
    check_failure(0, 1, 0, 1);
    return 0;
}
