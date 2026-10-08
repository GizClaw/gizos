#include "h2_loader_ble.h"
#include "h2_desktop_platform.h"
#include "libs/pal/providers/desktop/pal_core/tests/h2_desktop_test_system_event.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t live;
static h2_pal_ble_adv_set_t *advertising;
static void *allocate(void *user, size_t size) {
    (void)user;
    void *ptr = malloc(size);
    if (ptr != NULL) ++live;
    return ptr;
}
static void release(void *user, void *ptr) {
    (void)user;
    if (ptr != NULL) { assert(live != 0u); --live; free(ptr); }
}
static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate, .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};
static int started(void *user, const h2_pal_system_event_t *event) {
    (void)user;
    if (event->payload != NULL) {
        assert(event->payload_size == sizeof(h2_pal_ble_adv_set_event_t));
        advertising = ((const h2_pal_ble_adv_set_event_t *)event->payload)->set;
    }
    return H2_PAL_OK;
}
static int session(void *user, h2_bleikcp_t *stream, uint16_t handle) {
    (void)user; (void)stream; (void)handle;
    assert(0 && "name regression does not create a connection");
    return H2_PAL_ERR_UNSUPPORTED;
}
static void expect_name(const char *name) {
    uint8_t bytes[H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN];
    size_t length = 0u;
    assert(advertising != NULL);
    assert(h2_desktop_platform_copy_ble_adv_set_data(
        advertising, bytes, sizeof(bytes), &length) == H2_PAL_OK);
    bool found = false, identity = false, uuid = false;
    for (size_t offset = 0u; offset < length;) {
        size_t field = bytes[offset];
        assert(field != 0u && field <= length - offset - 1u);
        uint8_t type = bytes[offset + 1u];
        const uint8_t *data = bytes + offset + 2u;
        size_t size = field - 1u;
        if (type == 0x09u) {
            assert(name != NULL && size == strlen(name));
            assert(memcmp(data, name, size) == 0);
            found = true;
        }
        if (type == 0x07u) uuid = true;
        if (type == 0x21u) {
            assert(size >= 31u && memcmp(data + 16u, "H2LD", 4u) == 0);
            assert(memcmp(data + 27u, "h200", 4u) == 0);
            identity = true;
        }
        offset += field + 1u;
    }
    assert(found == (name != NULL) && identity && uuid);
}

int main(void) {
    assert(h2_desktop_test_system_event_init() == H2_PAL_OK);
    const h2_pal_system_event_api_t *events = h2_desktop_test_system_event_api();
    h2_pal_ble_t *ble = h2_desktop_platform_ble(events);
    assert(ble != NULL);
    assert(h2_desktop_platform_configure_ble_extended_advertising(1) == H2_PAL_OK);
    assert(h2_bleikcp_global_init() == H2_PAL_OK);
    h2_pal_system_event_subscription_t *subscription = NULL;
    assert(h2_pal_system_event_subscribe(events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
                                         started, NULL, &subscription) == H2_PAL_OK);
    char name[] = "H200-38DE";
    h2_loader_ble_service_config_t config = {
        .api = {.ble = ble, .task = h2_desktop_platform_task_api(),
            .time = h2_desktop_platform_time_api(), .sync = h2_desktop_platform_sync_api(),
            .system_event = events, .allocator = &memory},
        .board = "h200", .capabilities = H2_LOADER_CAPABILITY_BLE,
        .advertising_mode = H2_LOADER_BLE_ADVERTISING_EXTENDED,
        .handler = session, .local_name = name,
    };
    h2_loader_ble_service_t *service = NULL;
    assert(h2_loader_ble_service_open(&config, &service) == H2_PAL_OK);
    expect_name("H200-38DE");
    memset(name, 'x', sizeof(name) - 1u);
    assert(h2_loader_ble_service_pause_advertising(service) == H2_PAL_OK);
    assert(h2_loader_ble_service_resume_advertising(service) == H2_PAL_OK);
    expect_name("H200-38DE");
    assert(h2_loader_ble_service_set_additional_advertised_services(service, NULL, 0u) == H2_PAL_OK);
    expect_name("H200-38DE");
    assert(h2_loader_ble_service_close(service) == H2_PAL_OK && live == 0u);
    config.local_name = NULL;
    assert(h2_loader_ble_service_open(&config, &service) == H2_PAL_OK);
    expect_name(NULL);
    assert(h2_loader_ble_service_close(service) == H2_PAL_OK && live == 0u);
    config.advertising_mode = H2_LOADER_BLE_ADVERTISING_LEGACY;
    config.board = "a-long-board-identity";
    config.local_name = "ABCDEFGHIJKLMN";
    assert(h2_loader_ble_service_open(&config, &service) == H2_PAL_ERR_INVALID_ARG);
    assert(service == NULL && live == 0u);
    h2_pal_system_event_unsubscribe(events, subscription);
    assert(h2_pal_ble_stop(ble) == H2_PAL_OK);
    assert(h2_bleikcp_global_shutdown() == H2_PAL_OK);
    h2_desktop_test_system_event_deinit();
    return 0;
}
