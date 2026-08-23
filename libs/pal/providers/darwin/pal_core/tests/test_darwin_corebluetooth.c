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

    assert(h2_darwin_corebluetooth_ble(NULL) == NULL);
    assert(h2_darwin_corebluetooth_ble(&incomplete_allocator) == NULL);
    h2_pal_ble_t *ble = h2_darwin_corebluetooth_ble(&allocator);
    assert(ble != NULL);
    assert(ble->allocator == &allocator);
    assert(h2_darwin_corebluetooth_ble(&allocator) == ble);
    assert(h2_darwin_corebluetooth_ble(&other_allocator) == NULL);
    assert(ble->allocator == &allocator);
    const h2_pal_ble_adv_data_t scan_response = {0};
    assert(h2_pal_ble_adv_set_set_scan_response_data(
               ble, (h2_pal_ble_adv_set_t *)ble, &scan_response) ==
           H2_PAL_ERR_UNSUPPORTED);
    return 0;
}
