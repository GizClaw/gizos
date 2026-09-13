#include "h2_h2loader_host.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
    int fail_mtu;
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
    if (!f->fail_mtu) return H2_PAL_ERR_TIMEOUT;
    *out_handle = 7u;
    return H2_PAL_OK;
}

static h2_pal_result_t exchange_mtu(
    void *user, uint16_t handle, uint16_t *out_mtu, uint32_t timeout) {
    (void)user;
    (void)out_mtu;
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

static void check_failure(int fail_mtu, int with_sink, int sink_fails) {
    fixture_t f = {0};
    f.fail_mtu = fail_mtu;
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
    /* These services are required by connect but must not be called before
     * the injected BLE/MTU failure. Empty vtables catch accidental use. */
    const h2_pal_task_api_t task = {0};
    const h2_pal_time_api_t time = {0};
    const h2_pal_sync_api_t sync = {0};
    const h2_pal_system_event_api_t event = {0};
    const h2_h2loader_host_ble_connection_config_t config = {
        .ble = &ble, .task = &task, .time = &time, .sync = &sync,
        .system_event = &event, .allocator = &memory,
        .address = {.type = H2_PAL_BLE_ADDR_TYPE_PUBLIC},
        .on_log = with_sink ? capture : NULL, .log_user = &f,
    };
    h2_h2loader_host_ble_connection_t *connection = NULL;
    h2_h2loader_host_status_t status;
    const h2_pal_result_t expected = fail_mtu ? H2_PAL_ERR_IO : H2_PAL_ERR_TIMEOUT;
    assert(h2_h2loader_host_ble_connect(&config, &connection, &status) == expected);
    assert(connection == NULL);
    assert(f.allocations == 1u && f.frees == 1u);
    assert(f.disconnects == (unsigned)fail_mtu);
    assert(f.logs == (unsigned)with_sink);
    if (with_sink) {
        char expected_line[96];
        (void)snprintf(expected_line, sizeof(expected_line),
                       "H2_BLE_HOST_DIAG stage=%s rc=%d\n",
                       fail_mtu ? "mtu" : "connect", expected);
        assert(strcmp(f.line, expected_line) == 0);
    }
}

int main(void) {
    for (int stage = 0; stage != 2; ++stage) {
        check_failure(stage, 0, 0);
        check_failure(stage, 1, 0);
        check_failure(stage, 1, 1);
    }
    return 0;
}
