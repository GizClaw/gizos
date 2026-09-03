#include "h2_ios_platform.h"
#include "h2_ios_corebluetooth_internal.h"

#include <assert.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>

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

typedef struct lifecycle_events {
    size_t started;
    size_t stopped;
} lifecycle_events_t;

typedef struct reentrant_connection_event {
    h2_pal_ble_t *ble;
    dispatch_semaphore_t completed;
    h2_pal_result_t operation_result;
    size_t observed;
} reentrant_connection_event_t;

static int observe_started(void *user, const h2_pal_system_event_t *event) {
    lifecycle_events_t *events = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED);
    ++events->started;
    return H2_PAL_OK;
}

static int observe_stopped(void *user, const h2_pal_system_event_t *event) {
    lifecycle_events_t *events = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED);
    ++events->stopped;
    return H2_PAL_OK;
}

static int observe_connected_and_use_ble(
    void *user,
    const h2_pal_system_event_t *event) {
    reentrant_connection_event_t *observed = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED);
    ++observed->observed;
    observed->operation_result =
        h2_pal_ble_unregister_gatt_services(observed->ble);
    dispatch_semaphore_signal(observed->completed);
    return H2_PAL_OK;
}

void h2_ios_test_corebluetooth(void) {
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

    assert(h2_ios_corebluetooth_ble(NULL) == NULL);
    assert(h2_ios_corebluetooth_ble(&incomplete_allocator) == NULL);
    h2_pal_ble_t *ble = h2_ios_corebluetooth_ble(&allocator);
    assert(ble != NULL);
    assert(ble->allocator == &allocator);
    assert(h2_ios_corebluetooth_ble(&allocator) == ble);
    assert(h2_ios_corebluetooth_ble(&other_allocator) == NULL);
    assert(ble->allocator == &allocator);
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

    assert(h2_ios_corebluetooth_readiness_result(0, 0, 0, 0) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_ios_corebluetooth_readiness_result(1, 1, 0, 0) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_ios_corebluetooth_readiness_result(1, 1, 1, 1) == H2_PAL_OK);
    assert(h2_ios_corebluetooth_readiness_result(1, 0, 1, 1) ==
           H2_PAL_ERR_UNAVAILABLE);
    assert(h2_ios_corebluetooth_readiness_result(1, 1, 1, 0) ==
           H2_PAL_ERR_UNAVAILABLE);

    const h2_pal_system_event_api_t *system_events =
        h2_ios_system_event_api();
    assert(h2_pal_system_event_init(system_events) == H2_PAL_OK);
    lifecycle_events_t observed = {0};
    h2_pal_system_event_subscription_t *started = NULL;
    h2_pal_system_event_subscription_t *stopped = NULL;
    assert(h2_pal_system_event_subscribe(
               system_events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
               observe_started, &observed, &started) == H2_PAL_OK);
    assert(h2_pal_system_event_subscribe(
               system_events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED,
               observe_stopped, &observed, &stopped) == H2_PAL_OK);

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
    h2_ios_corebluetooth_test_post_connected_on_backend_queue();
    assert(dispatch_semaphore_wait(
               reentrant.completed,
               dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0);
    assert(reentrant.observed == 1u);
    assert(reentrant.operation_result == H2_PAL_OK);
    h2_pal_system_event_unsubscribe(system_events, connected);

    h2_ios_corebluetooth_test_set_start_result(1, H2_PAL_OK);
    for (size_t iteration = 0u; iteration < 2u; ++iteration) {
        assert(h2_pal_ble_start(ble) == H2_PAL_OK);
        assert(h2_pal_ble_start(ble) == H2_PAL_ERR_INVALID_STATE);
        assert(h2_pal_ble_stop(ble) == H2_PAL_OK);
        assert(h2_pal_ble_stop(ble) == H2_PAL_OK);
    }
    assert(observed.started == 2u);
    assert(observed.stopped == 2u);

    h2_ios_corebluetooth_test_set_start_result(
        1, H2_PAL_ERR_UNAVAILABLE);
    assert(h2_pal_ble_start(ble) == H2_PAL_ERR_UNAVAILABLE);
    assert(h2_pal_ble_stop(ble) == H2_PAL_OK);
    assert(observed.started == 2u);
    assert(observed.stopped == 2u);
    h2_ios_corebluetooth_test_set_start_result(0, H2_PAL_OK);

    h2_pal_system_event_unsubscribe(system_events, stopped);
    h2_pal_system_event_unsubscribe(system_events, started);
    h2_pal_system_event_deinit(system_events);
}
