#include "h2_loader_ble.h"
#include "h2_desktop_platform.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct advertising_capture {
    char name[30];
    size_t manufacturer_len;
    size_t identity_len;
    unsigned updates;
    int set_marker;
} advertising_capture_t;

struct h2_pal_system_event_subscription { unsigned marker; };
static h2_pal_result_t subscribe(void *user, h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler, void *handler_user,
    h2_pal_system_event_subscription_t **out) {
    (void)user; (void)type; (void)handler; (void)handler_user;
    *out = calloc(1u, sizeof(**out));
    return *out != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}
static void unsubscribe(void *user, h2_pal_system_event_subscription_t *value) {
    (void)user; free(value);
}

static h2_pal_result_t ok(void *user) { (void)user; return H2_PAL_OK; }
static h2_pal_result_t register_services(void *user,
    const h2_pal_ble_gatt_service_t *services, size_t count) {
    (void)user;
    assert(count == 1u);
    if (services->out_service_handle != NULL) *services->out_service_handle = 1u;
    for (size_t i = 0u; i < services->characteristic_count; ++i) {
        const h2_pal_ble_gatt_characteristic_t *value = &services->characteristics[i];
        if (value->out_value_handle != NULL) *value->out_value_handle = (uint16_t)(i + 2u);
        if (value->out_cccd_handle != NULL) *value->out_cccd_handle = 4u;
    }
    return H2_PAL_OK;
}
static h2_pal_result_t unregister_service(void *user, const h2_pal_ble_uuid_t *uuid) {
    (void)user; (void)uuid; return H2_PAL_OK;
}
static h2_pal_result_t capture_data(void *user, const h2_pal_ble_adv_data_t *data) {
    advertising_capture_t *capture = user;
    assert(data->service_uuid_count == 1u);
    assert(data->service_uuids[0].len == 16u);
    assert(memcmp(data->service_uuids[0].data, h2_loader_ble_service_uuid_bytes, 16u) == 0);
    assert(data->local_name == NULL || strlen(data->local_name) < sizeof(capture->name));
    strcpy(capture->name, data->local_name != NULL ? data->local_name : "");
    capture->manufacturer_len = data->manufacturer_data.len;
    capture->identity_len = data->service_data.len;
    ++capture->updates;
    return H2_PAL_OK;
}
static h2_pal_result_t start_advertising(void *user, const h2_pal_ble_adv_params_t *params) {
    (void)user;
    assert(params->type == H2_PAL_BLE_ADV_TYPE_LEGACY);
    return H2_PAL_OK;
}
static h2_pal_result_t create_set(void *user, const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out) {
    advertising_capture_t *capture = user;
    assert(params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED);
    *out = (h2_pal_ble_adv_set_t *)&capture->set_marker;
    return H2_PAL_OK;
}
static h2_pal_result_t set_data(void *user, h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    advertising_capture_t *capture = user;
    assert(set == (h2_pal_ble_adv_set_t *)&capture->set_marker);
    return capture_data(user, data);
}
static h2_pal_result_t set_ok(void *user, h2_pal_ble_adv_set_t *set) {
    (void)user; (void)set; return H2_PAL_OK;
}
static int no_session(void *user, h2_bleikcp_t *stream, uint16_t handle) {
    (void)user; (void)stream; (void)handle;
    assert(0 && "no physical peer in this test");
    return H2_PAL_ERR_UNSUPPORTED;
}

static void test_mode(h2_loader_ble_advertising_mode_t mode, bool named) {
    advertising_capture_t capture = {0};
    const h2_pal_ble_vtable_t ops = {
        .start = ok, .stop = ok, .register_gatt_services = register_services,
        .unregister_gatt_service = unregister_service, .unregister_gatt_services = ok,
        .set_adv_data = capture_data, .start_advertising = start_advertising,
        .stop_advertising = ok, .adv_set_create = create_set,
        .adv_set_set_data = set_data, .adv_set_start = set_ok,
        .adv_set_stop = set_ok, .adv_set_destroy = set_ok,
    };
    const h2_pal_ble_host_api_t ble = {.user = &capture, .vtable = &ops};
    const h2_pal_system_event_vtable_t event_ops = {
        .subscribe = subscribe, .unsubscribe = unsubscribe,
    };
    const h2_pal_system_event_api_t events = {.vtable = &event_ops};
    char name[] = "h106-tiga-A1B2C3";
    h2_loader_ble_service_config_t config = {
        .api = {.ble = &ble,
            .task = h2_desktop_platform_task_api(),
            .time = h2_desktop_platform_time_api(),
            .sync = h2_desktop_platform_sync_api(),
            .system_event = &events,
            .allocator = h2_desktop_platform_default_allocator()},
        .board = "tiga_esp_v4_2", .local_name = named ? name : NULL,
        .advertising_mode = mode, .handler = no_session,
    };
    h2_loader_ble_service_t *service = NULL;
    assert(h2_loader_ble_service_open(&config, &service) == H2_PAL_OK);
    assert(strcmp(capture.name, named ? "h106-tiga-A1B2C3" : "") == 0);
    assert(capture.manufacturer_len != 0u || named || mode != H2_LOADER_BLE_ADVERTISING_LEGACY);
    if (named && mode == H2_LOADER_BLE_ADVERTISING_LEGACY)
        assert(capture.manufacturer_len == 0u && capture.identity_len == 0u);
    if (mode == H2_LOADER_BLE_ADVERTISING_EXTENDED)
        assert(capture.identity_len != 0u);
    memset(name, 'X', sizeof(name) - 1u);
    assert(h2_loader_ble_service_pause_advertising(service) == H2_PAL_OK);
    assert(h2_loader_ble_service_resume_advertising(service) == H2_PAL_OK);
    assert(capture.updates >= 2u);
    assert(strcmp(capture.name, named ? "h106-tiga-A1B2C3" : "") == 0);
    assert(h2_loader_ble_service_close(service) == H2_PAL_OK);
    config.local_name = "bad\nname";
    assert(h2_loader_ble_service_open(&config, &service) == H2_PAL_ERR_INVALID_ARG);
    assert(service == NULL);
    config.local_name = "012345678901234567890123456789";
    assert(h2_loader_ble_service_open(&config, &service) == H2_PAL_ERR_INVALID_ARG);
    assert(service == NULL);
}

int main(void) {
    assert(h2_bleikcp_global_init() == H2_PAL_OK);
    test_mode(H2_LOADER_BLE_ADVERTISING_LEGACY, false);
    test_mode(H2_LOADER_BLE_ADVERTISING_LEGACY, true);
    test_mode(H2_LOADER_BLE_ADVERTISING_EXTENDED, true);
    assert(h2_bleikcp_global_shutdown() == H2_PAL_OK);
    return 0;
}
