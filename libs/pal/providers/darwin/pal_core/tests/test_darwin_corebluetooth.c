#include "h2_darwin_platform.h"

#include <assert.h>
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

static int test_log(void *user, h2_pal_log_level_t level,
                    const char *scope, const char *message) {
    (void)user;
    (void)level;
    (void)scope;
    (void)message;
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
    return 0;
}
