#include "h2_bk3633_platform_core.h"
#include "h2_bk3633_ble_sdk_fake.h"
#include "h2_bk3633_mem_test_support.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static unsigned s_read_calls;
static unsigned s_write_calls;
static uint8_t s_last_write[16];
static size_t s_last_write_len;

static h2_pal_result_t time_now(void *user, uint64_t *out_ms) {
    (void)user;
    *out_ms = 0u;
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t s_time_vtable = {
    .get_monotonic_ms = time_now,
};

static const h2_pal_time_api_t s_time_api = {
    .vtable = &s_time_vtable,
};

static h2_pal_result_t read_value(
    void *user, const h2_pal_ble_gatt_access_t *access,
    uint8_t *out, size_t out_size, size_t *out_len) {
    (void)user;
    assert(access->offset == 0u);
    assert(out_size >= 2u);
    ++s_read_calls;
    out[0] = 0xa5u;
    out[1] = 0x5au;
    *out_len = 2u;
    return H2_PAL_OK;
}

static h2_pal_result_t write_value(
    void *user, const h2_pal_ble_gatt_access_t *access,
    const uint8_t *data, size_t len) {
    (void)user;
    assert(access->offset == 0u);
    assert(len <= sizeof(s_last_write));
    ++s_write_calls;
    memcpy(s_last_write, data, len);
    s_last_write_len = len;
    return H2_PAL_OK;
}

static void start_provider(void) {
    h2_bk3633_ble_sdk_fake_reset();
    h2_bk3633_platform_ble_test_reset();
    const h2_bk3633_platform_ble_config_t config = {
        .mem = h2_bk3633_platform_mem_api(),
        .time = &s_time_api,
        .gatt_pending_access_capacity = 2u,
        .bootstrap_timeout_ms = 100u,
    };
    assert(h2_bk3633_platform_ble_configure(&config) == H2_PAL_OK);
    assert(h2_bk3633_platform_ble_host_bootstrap_begin() == H2_PAL_OK);
    h2_bk3633_platform_ble_host_bootstrap_complete(H2_PAL_OK);
    s_read_calls = 0u;
    s_write_calls = 0u;
    s_last_write_len = 0u;
}

static void connect_peer(uint16_t conn_handle) {
    const struct gapc_connection_req_ind connected = {
        .conhdl = conn_handle,
        .role = 1u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPC_CONNECTION_REQ_IND, &connected, TASK_APP,
        KE_BUILD_ID(TASK_GAPC, 1u));
}

int main(void) {
    h2_bk3633_mem_test_support_init();
    start_provider();
    const h2_pal_ble_host_api_t *api = h2_bk3633_platform_ble_api();
    const uint8_t service16_uuid[] = {0x0fu, 0x18u};
    const uint8_t characteristic16_uuid[] = {0x19u, 0x2au};
    const uint8_t service128_uuid[16] = {
        0x02u, 0x00u, 0x30u, 0x6bu, 0x00u, 0xefu, 0x5bu, 0x4au,
        0x8du, 0xc1u, 0x2eu, 0x9fu, 0xcdu, 0xefu, 0x00u, 0x02u,
    };
    const uint8_t characteristic128_uuid[16] = {
        0x23u, 0x00u, 0x30u, 0x6bu, 0x00u, 0xefu, 0x5bu, 0x4au,
        0x8du, 0xc1u, 0x2eu, 0x9fu, 0xcdu, 0xefu, 0x00u, 0x02u,
    };
    uint16_t service16_handle = 0u;
    uint16_t value16_handle = 0u;
    uint16_t cccd16_handle = 0u;
    uint16_t service128_handle = 0u;
    uint16_t value128_handle = 0u;
    uint16_t cccd128_handle = 0u;
    const h2_pal_ble_gatt_characteristic_t characteristic16 = {
        .uuid = {.data = characteristic16_uuid,
                 .len = sizeof(characteristic16_uuid)},
        .properties = H2_PAL_BLE_GATT_PROPERTY_READ |
                      H2_PAL_BLE_GATT_PROPERTY_WRITE |
                      H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
        .permissions = H2_PAL_BLE_GATT_PERMISSION_READ |
                       H2_PAL_BLE_GATT_PERMISSION_WRITE,
        .max_value_len = 16u,
        .read = read_value,
        .write = write_value,
        .out_value_handle = &value16_handle,
        .out_cccd_handle = &cccd16_handle,
    };
    const h2_pal_ble_gatt_characteristic_t characteristic128 = {
        .uuid = {.data = characteristic128_uuid,
                 .len = sizeof(characteristic128_uuid)},
        .properties = H2_PAL_BLE_GATT_PROPERTY_READ |
                      H2_PAL_BLE_GATT_PROPERTY_INDICATE,
        .permissions = H2_PAL_BLE_GATT_PERMISSION_READ,
        .max_value_len = 16u,
        .read = read_value,
        .out_value_handle = &value128_handle,
        .out_cccd_handle = &cccd128_handle,
    };
    const h2_pal_ble_gatt_service_t services[] = {
        {
            .uuid = {.data = service16_uuid, .len = sizeof(service16_uuid)},
            .primary = true,
            .characteristics = &characteristic16,
            .characteristic_count = 1u,
            .out_service_handle = &service16_handle,
        },
        {
            .uuid = {.data = service128_uuid,
                     .len = sizeof(service128_uuid)},
            .primary = false,
            .characteristics = &characteristic128,
            .characteristic_count = 1u,
            .out_service_handle = &service128_handle,
        },
    };
    assert(h2_pal_ble_register_gatt_services(api, services, 2u) == H2_PAL_OK);
    assert(h2_bk3633_ble_sdk_fake_service_count() == 2u);
    assert(h2_bk3633_ble_sdk_fake_service(0u)->uuid_len == 2u);
    assert(h2_bk3633_ble_sdk_fake_service(1u)->uuid_len == 16u);
    assert((h2_bk3633_ble_sdk_fake_service(0u)->permissions &
            (uint8_t)PERM(SVC_DIS, ENABLE)) == 0u);
    assert((h2_bk3633_ble_sdk_fake_service(0u)->permissions &
            (uint8_t)PERM(SVC_SECONDARY, ENABLE)) == 0u);
    assert((h2_bk3633_ble_sdk_fake_service(0u)->permissions &
            (uint8_t)PERM(SVC_UUID_LEN, UUID_128)) == 0u);
    assert((h2_bk3633_ble_sdk_fake_service(1u)->permissions &
            (uint8_t)PERM(SVC_DIS, ENABLE)) == 0u);
    assert((h2_bk3633_ble_sdk_fake_service(1u)->permissions &
            (uint8_t)PERM(SVC_SECONDARY, ENABLE)) != 0u);
    assert((h2_bk3633_ble_sdk_fake_service(1u)->permissions &
            (uint8_t)PERM(SVC_UUID_LEN, UUID_128)) != 0u);
    assert(memcmp(h2_bk3633_ble_sdk_fake_service(1u)->uuid,
                  service128_uuid, sizeof(service128_uuid)) == 0);
    assert(h2_bk3633_ble_sdk_fake_service(1u)->attribute_uuid_len[0u] == 2u);
    assert(h2_bk3633_ble_sdk_fake_service(1u)->attribute_uuid_len[1u] == 2u);
    assert(h2_bk3633_ble_sdk_fake_service(1u)->attribute_uuid_len[2u] == 16u);
    assert(h2_bk3633_ble_sdk_fake_service(1u)->attribute_uuid_len[3u] == 2u);
    assert(!h2_bk3633_ble_sdk_fake_service(0u)->hidden);
    assert(!h2_bk3633_ble_sdk_fake_service(1u)->hidden);
    assert(service16_handle != 0u && service128_handle != 0u);
    assert(value16_handle == service16_handle + 2u);
    assert(cccd16_handle == service16_handle + 3u);
    assert(value128_handle == service128_handle + 2u);
    assert(cccd128_handle == service128_handle + 3u);

    connect_peer(42u);
    const struct gattc_read_req_ind read_request = {
        .handle = value16_handle,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_READ_REQ_IND, &read_request, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    assert(s_read_calls == 0u);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_read_calls == 1u);

    union {
        max_align_t alignment;
        uint8_t bytes[sizeof(struct gattc_write_req_ind) + 3u];
    } write_storage = {.bytes = {0}};
    struct gattc_write_req_ind *write_request =
        (struct gattc_write_req_ind *)write_storage.bytes;
    write_request->handle = value16_handle;
    write_request->length = 3u;
    write_request->value[0] = 1u;
    write_request->value[1] = 2u;
    write_request->value[2] = 3u;
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_WRITE_REQ_IND, write_request, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    memset(write_request->value, 0xff, 3u);
    assert(s_write_calls == 0u);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_write_calls == 1u);
    assert(s_last_write_len == 3u);
    assert(s_last_write[0] == 1u && s_last_write[2] == 3u);

    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_WRITE_REQ_IND, write_request, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    assert(h2_pal_ble_unregister_gatt_services(api) == H2_PAL_OK);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_write_calls == 1u);
    assert(h2_bk3633_ble_sdk_fake_service(0u)->hidden);
    assert(h2_bk3633_ble_sdk_fake_service(1u)->hidden);
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_READ_REQ_IND, &read_request, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_read_calls == 1u);

    start_provider();
    h2_bk3633_ble_sdk_fake_fail_service_create(2u);
    service16_handle = value16_handle = cccd16_handle = 0u;
    service128_handle = value128_handle = cccd128_handle = 0u;
    assert(h2_pal_ble_register_gatt_services(api, services, 2u) ==
           H2_PAL_ERR_IO);
    assert(h2_bk3633_ble_sdk_fake_service_count() == 1u);
    assert(h2_bk3633_ble_sdk_fake_service(0u)->hidden);
    assert(service16_handle == 0u && service128_handle == 0u);

    start_provider();
    h2_bk3633_ble_sdk_fake_fail_service_visibility(1u);
    service16_handle = value16_handle = cccd16_handle = 7u;
    service128_handle = value128_handle = cccd128_handle = 7u;
    assert(h2_pal_ble_register_gatt_services(api, services, 2u) == H2_PAL_OK);
    assert(h2_bk3633_ble_sdk_fake_service_count() == 2u);
    assert(!h2_bk3633_ble_sdk_fake_service(0u)->hidden);
    assert(!h2_bk3633_ble_sdk_fake_service(1u)->hidden);
    assert(service16_handle != H2_PAL_BLE_INVALID_ATTR_HANDLE);
    assert(service128_handle != H2_PAL_BLE_INVALID_ATTR_HANDLE);
    assert(h2_pal_ble_unregister_gatt_services(api) == H2_PAL_ERR_IO);

    start_provider();
    uint16_t mixed_service_handle = 0u;
    uint16_t mixed_value16_handle = 0u;
    uint16_t mixed_cccd16_handle = 0u;
    uint16_t mixed_value128_handle = 0u;
    uint16_t mixed_cccd128_handle = 0u;
    uint16_t mixed_service128_handle = 0u;
    uint16_t mixed_value16_in_128_handle = 0u;
    uint16_t mixed_cccd16_in_128_handle = 0u;
    h2_pal_ble_gatt_characteristic_t mixed_characteristics[] = {
        characteristic16,
        characteristic128,
    };
    mixed_characteristics[0].out_value_handle = &mixed_value16_handle;
    mixed_characteristics[0].out_cccd_handle = &mixed_cccd16_handle;
    mixed_characteristics[1].out_value_handle = &mixed_value128_handle;
    mixed_characteristics[1].out_cccd_handle = &mixed_cccd128_handle;
    h2_pal_ble_gatt_characteristic_t characteristic16_in_128 =
        characteristic16;
    characteristic16_in_128.out_value_handle =
        &mixed_value16_in_128_handle;
    characteristic16_in_128.out_cccd_handle =
        &mixed_cccd16_in_128_handle;
    const h2_pal_ble_gatt_service_t mixed_services[] = {
        {
            .uuid = {.data = service16_uuid, .len = sizeof(service16_uuid)},
            .primary = true,
            .characteristics = mixed_characteristics,
            .characteristic_count = 2u,
            .out_service_handle = &mixed_service_handle,
        },
        {
            .uuid = {.data = service128_uuid,
                     .len = sizeof(service128_uuid)},
            .primary = true,
            .characteristics = &characteristic16_in_128,
            .characteristic_count = 1u,
            .out_service_handle = &mixed_service128_handle,
        },
    };
    assert(h2_pal_ble_register_gatt_services(api, mixed_services, 2u) ==
           H2_PAL_OK);
    const uint8_t expanded_service16_uuid[16] = {
        0xfbu, 0x34u, 0x9bu, 0x5fu, 0x80u, 0x00u, 0x00u, 0x80u,
        0x00u, 0x10u, 0x00u, 0x00u, 0x0fu, 0x18u, 0x00u, 0x00u,
    };
    const h2_bk3633_ble_sdk_fake_service_t *mixed_sdk_service =
        h2_bk3633_ble_sdk_fake_service(0u);
    assert(mixed_sdk_service != NULL);
    assert(mixed_sdk_service->uuid_len == 16u);
    assert((mixed_sdk_service->permissions &
            (uint8_t)PERM(SVC_UUID_LEN, UUID_128)) != 0u);
    assert(memcmp(mixed_sdk_service->uuid, expanded_service16_uuid,
                  sizeof(expanded_service16_uuid)) == 0);
    assert(mixed_sdk_service->attribute_count == 7u);
    assert(mixed_sdk_service->attribute_uuid_len[0u] == 2u);
    assert(mixed_sdk_service->attribute_uuid[0u][0u] == 0x00u);
    assert(mixed_sdk_service->attribute_uuid[0u][1u] == 0x28u);
    assert(mixed_sdk_service->attribute_uuid_len[1u] == 2u);
    assert(mixed_sdk_service->attribute_uuid[1u][0u] == 0x03u);
    assert(mixed_sdk_service->attribute_uuid[1u][1u] == 0x28u);
    assert((mixed_sdk_service->attribute_ext_permissions[2u] &
            PERM(UUID_LEN, UUID_128)) == 0u);
    assert(mixed_sdk_service->attribute_uuid_len[2u] == 2u);
    assert(memcmp(mixed_sdk_service->attribute_uuid[2u],
                  characteristic16_uuid,
                  sizeof(characteristic16_uuid)) == 0);
    assert(mixed_sdk_service->attribute_ext_permissions[5u] ==
           (PERM(UUID_LEN, UUID_128) | PERM(RI, ENABLE)));
    assert(mixed_sdk_service->attribute_uuid_len[5u] == 16u);
    assert(memcmp(mixed_sdk_service->attribute_uuid[5u],
                  characteristic128_uuid,
                  sizeof(characteristic128_uuid)) == 0);
    assert(mixed_service_handle != 0u);
    assert(mixed_value16_handle == mixed_service_handle + 2u);
    assert(mixed_cccd16_handle == mixed_service_handle + 3u);
    assert(mixed_value128_handle == mixed_service_handle + 5u);
    assert(mixed_cccd128_handle == mixed_service_handle + 6u);
    const h2_bk3633_ble_sdk_fake_service_t *mixed_sdk_service128 =
        h2_bk3633_ble_sdk_fake_service(1u);
    assert(mixed_sdk_service128 != NULL);
    assert((mixed_sdk_service128->permissions &
            (uint8_t)PERM(SVC_UUID_LEN, UUID_128)) != 0u);
    assert(memcmp(mixed_sdk_service128->uuid, service128_uuid,
                  sizeof(service128_uuid)) == 0);
    assert(mixed_sdk_service128->attribute_uuid_len[0u] == 2u);
    assert(mixed_sdk_service128->attribute_uuid_len[1u] == 2u);
    assert(mixed_sdk_service128->attribute_uuid_len[2u] == 2u);
    assert(memcmp(mixed_sdk_service128->attribute_uuid[2u],
                  characteristic16_uuid,
                  sizeof(characteristic16_uuid)) == 0);
    assert(mixed_value16_in_128_handle == mixed_service128_handle + 2u);
    assert(mixed_cccd16_in_128_handle == mixed_service128_handle + 3u);

    start_provider();
    assert(h2_pal_ble_register_gatt_services(api, services, 2u) == H2_PAL_OK);
    connect_peer(43u);
    h2_bk3633_ble_sdk_fake_fail_service_visibility(1u);
    assert(h2_pal_ble_unregister_gatt_services(api) == H2_PAL_ERR_IO);
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_READ_REQ_IND, &read_request, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_read_calls == 0u);
    h2_bk3633_platform_ble_test_reset();
    return 0;
}
