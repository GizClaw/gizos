#include "h2_darwin_platform.h"
#include "h2_darwin_corebluetooth_internal.h"

#include <assert.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

typedef struct connect_retry {
    h2_pal_ble_t *ble;
    h2_pal_ble_addr_t address;
    dispatch_semaphore_t completed;
    h2_pal_result_t result;
} connect_retry_t;

static uint64_t monotonic_ms(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void *connect_retry_thread(void *user) {
    connect_retry_t *retry = user;
    const h2_pal_ble_connect_params_t params = {.timeout_ms = 2000u};
    uint16_t handle = 0u;
    retry->result = h2_pal_ble_connect(
        retry->ble, &retry->address, &params, &handle);
    dispatch_semaphore_signal(retry->completed);
    return NULL;
}

typedef struct reentrant_connection_event {
    h2_pal_ble_t *ble;
    dispatch_semaphore_t completed;
    h2_pal_result_t operation_result;
    size_t observed;
} reentrant_connection_event_t;

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

static bool ignore_scan(
    void *user, const h2_pal_ble_scan_result_t *result) {
    (void)user;
    (void)result;
    return false;
}

static int test_log(void *user, h2_pal_log_level_t level,
                    const char *scope, const char *message) {
    (void)user;
    (void)level;
    (void)scope;
    (void)message;
    return H2_PAL_OK;
}

static int observe_connected_and_use_ble(
    void *user,
    const h2_pal_system_event_t *event) {
    reentrant_connection_event_t *observed = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED);
    assert(event->payload_size == sizeof(h2_pal_ble_connection_t));
    const h2_pal_ble_connection_t *connection = event->payload;
    assert(connection->conn_handle == 1u);
    assert(connection->mtu == 23u);
    ++observed->observed;
    observed->operation_result =
        h2_pal_ble_unregister_gatt_services(observed->ble);
    dispatch_semaphore_signal(observed->completed);
    return H2_PAL_OK;
}

int main(void) {
    static const h2_pal_mem_vtable_t complete_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
    };
    static const h2_pal_mem_vtable_t incomplete_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
    };
    static const h2_pal_mem_api_t allocator = {
        .user = NULL,
        .vtable = &complete_vtable,
    };
    static const h2_pal_mem_api_t other_allocator = {
        .user = NULL,
        .vtable = &complete_vtable,
    };
    static const h2_pal_mem_api_t incomplete_allocator = {
        .user = NULL,
        .vtable = &incomplete_vtable,
    };

    static const h2_pal_log_vtable_t log_vtable = {.write = test_log};
    static const h2_pal_log_vtable_t empty_log_vtable = {0};
    static const h2_pal_log_api_t log = {.vtable = &log_vtable};
    static const h2_pal_log_api_t other_log = {.vtable = &log_vtable};
    static const h2_pal_log_api_t incomplete_log = {.vtable = &empty_log_vtable};
    assert(h2_darwin_corebluetooth_ble(NULL, &log) == NULL);
    assert(h2_darwin_corebluetooth_ble(&incomplete_allocator, &log) == NULL);
    assert(h2_darwin_corebluetooth_ble(&allocator, NULL) == NULL);
    assert(h2_darwin_corebluetooth_ble(&allocator, &incomplete_log) == NULL);
    h2_pal_ble_t *ble = h2_darwin_corebluetooth_ble(&allocator, &log);
    assert(ble != NULL);
    assert(ble->allocator == &allocator);
    assert(h2_darwin_corebluetooth_ble(&allocator, &log) == ble);
    assert(h2_darwin_corebluetooth_ble(&other_allocator, &log) == NULL);
    assert(h2_darwin_corebluetooth_ble(&allocator, &other_log) == NULL);
    assert(h2_darwin_corebluetooth_ble(&allocator, &log) == ble);
    assert(ble->allocator == &allocator);
    const h2_pal_ble_addr_t timeout_address = {0};
    const h2_pal_ble_connect_params_t timeout_params = {.timeout_ms = 20u};
    uint16_t connection_handle = 0u;
    h2_darwin_corebluetooth_test_set_pending_connect(&timeout_address);
    assert(!h2_darwin_corebluetooth_test_connect_cleanup(0u));
    for (unsigned attempt = 1u; attempt <= 2u; ++attempt) {
        assert(h2_pal_ble_connect(ble, &timeout_address, &timeout_params,
                                  &connection_handle) == H2_PAL_ERR_TIMEOUT);
        assert(h2_darwin_corebluetooth_test_connect_cleanup(attempt));
    }
    h2_darwin_corebluetooth_test_deliver_central_event(
        H2_DARWIN_COREBLUETOOTH_TEST_CONNECTED, 0);
    assert(h2_darwin_corebluetooth_test_connect_cleanup(3u));
    assert(h2_pal_ble_connect(ble, &timeout_address, &timeout_params,
                              &connection_handle) == H2_PAL_ERR_TIMEOUT);
    assert(h2_darwin_corebluetooth_test_connect_cleanup(4u));

    /* A late failure cannot be distinguished from this same-object retry. */
    connect_retry_t retry = {
        .ble = ble,
        .address = timeout_address,
        .completed = dispatch_semaphore_create(0),
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    pthread_t retry_thread;
    assert(pthread_create(&retry_thread, NULL, connect_retry_thread, &retry) == 0);
    uint64_t pending_deadline = monotonic_ms() + 5000u;
    while (!h2_darwin_corebluetooth_test_connect_pending()) {
        assert(monotonic_ms() < pending_deadline);
        const struct timespec pause = {.tv_nsec = 1000000};
        (void)nanosleep(&pause, NULL);
    }
    h2_darwin_corebluetooth_test_deliver_central_event(
        H2_DARWIN_COREBLUETOOTH_TEST_FAILED, 0);
    assert(dispatch_semaphore_wait(retry.completed, DISPATCH_TIME_FOREVER) == 0);
    assert(pthread_join(retry_thread, NULL) == 0);
    assert(retry.result == H2_PAL_ERR_IO);
    assert(h2_darwin_corebluetooth_test_connect_cleanup(5u));
    dispatch_release(retry.completed);

    h2_darwin_corebluetooth_test_set_other_connected();
    assert(h2_darwin_corebluetooth_test_other_connected());
    h2_darwin_corebluetooth_test_deliver_central_event(
        H2_DARWIN_COREBLUETOOTH_TEST_FAILED, 0);
    assert(h2_darwin_corebluetooth_test_other_connected());
    h2_darwin_corebluetooth_test_deliver_central_event(
        H2_DARWIN_COREBLUETOOTH_TEST_DISCONNECTED, 0);
    assert(h2_darwin_corebluetooth_test_other_connected());
    h2_darwin_corebluetooth_test_set_pending_connect(NULL);

    const h2_pal_ble_adv_data_t scan_response = {0};
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               ble, (h2_pal_ble_adv_set_t *)ble, &scan_response) ==
           H2_PAL_ERR_UNSUPPORTED);
    const uint8_t encoded[] = { 2u, 0xffu, 1u };
    assert(h2_pal_ble_adv_set_set_encoded_data(
               ble, (h2_pal_ble_adv_set_t *)ble, encoded,
               sizeof(encoded)) == H2_PAL_ERR_UNSUPPORTED);
    const h2_pal_ble_scan_params_t exact_scan = {
        .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE,
        .type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
        .interval_units_625us = 4u,
        .window_units_625us = 4u,
    };
    assert(h2_pal_ble_start_scan(
               ble, &exact_scan, ignore_scan, NULL) ==
           H2_PAL_ERR_UNSUPPORTED);

    const h2_pal_system_event_api_t *system_events =
        h2_darwin_system_event_api();
    assert(h2_pal_system_event_init(system_events) == H2_PAL_OK);
    reentrant_connection_event_t reentrant = {
        .ble = ble,
        .completed = dispatch_semaphore_create(0),
        .operation_result = H2_PAL_ERR_INVALID_STATE,
    };
    h2_pal_system_event_subscription_t *connected = NULL;
    assert(h2_pal_system_event_subscribe(
               system_events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
               observe_connected_and_use_ble, &reentrant, &connected) ==
           H2_PAL_OK);
    h2_darwin_corebluetooth_test_post_connected_on_backend_queue();
    assert(dispatch_semaphore_wait(
               reentrant.completed,
               dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0);
    assert(reentrant.observed == 1u);
    assert(reentrant.operation_result == H2_PAL_OK);
    h2_pal_system_event_unsubscribe(system_events, connected);
    h2_pal_system_event_deinit(system_events);
    return 0;
}
