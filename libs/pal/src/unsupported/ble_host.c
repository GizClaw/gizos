#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_ble_host_start(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_stop(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_set_adv_data(void *p0, const h2_pal_ble_adv_data_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_start_advertising(void *p0, const h2_pal_ble_adv_params_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_stop_advertising(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_adv_set_create(
    void *p0,
    const h2_pal_ble_adv_params_t *p1,
    h2_pal_ble_adv_set_t **p2) {
    (void)p0;
    (void)p1;
    if (p2 != NULL) {
        *p2 = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_adv_set_set_data(
    void *p0,
    h2_pal_ble_adv_set_t *p1,
    const h2_pal_ble_adv_data_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_adv_set_set_encoded_data(
    void *p0,
    h2_pal_ble_adv_set_t *p1,
    const uint8_t *p2,
    size_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_adv_set_set_scan_response_data(
    void *p0,
    h2_pal_ble_adv_set_t *p1,
    const h2_pal_ble_adv_data_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_adv_set_operation(
    void *p0,
    h2_pal_ble_adv_set_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_start_scan(void *p0, const h2_pal_ble_scan_params_t *p1, h2_pal_ble_scan_result_fn p2, void *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_stop_scan(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_register_gatt_services(void *p0, const h2_pal_ble_gatt_service_t *p1, size_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_unregister_gatt_services(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_notify(void *p0, uint16_t p1, uint16_t p2, const uint8_t *p3, size_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_indicate(
    void *p0, uint16_t p1, uint16_t p2, const uint8_t *p3, size_t p4,
    uint32_t p5) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_connect(void *p0, const h2_pal_ble_addr_t *p1, const h2_pal_ble_connect_params_t *p2, uint16_t *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_disconnect(void *p0, uint16_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_update_connection(void *p0, uint16_t p1, const h2_pal_ble_connection_params_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_exchange_mtu(void *p0, uint16_t p1, uint16_t *p2, uint32_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_set_preferred_phy(void *p0, uint16_t p1, h2_pal_ble_phy_t p2, h2_pal_ble_phy_t p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_read_phy(void *p0, uint16_t p1, h2_pal_ble_phy_info_t *p2, uint32_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_gatt_discover(void *p0, uint16_t p1, const h2_pal_ble_gatt_discovery_request_t *p2, h2_pal_ble_gatt_discovery_entry_t *p3, size_t p4, size_t *p5, uint32_t p6) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_gatt_read(void *p0, uint16_t p1, uint16_t p2, uint16_t p3, uint8_t *p4, size_t p5, size_t *p6, uint32_t p7) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    (void)p7;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_gatt_write(void *p0, uint16_t p1, uint16_t p2, const uint8_t *p3, size_t p4, _Bool p5, uint32_t p6) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_ble_host_gatt_subscribe(void *p0, uint16_t p1, const h2_pal_ble_gatt_subscribe_t *p2, uint32_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_ble_vtable_t unsupported_ble_host_vtable = {
    .start = unsupported_ble_host_start,
    .stop = unsupported_ble_host_stop,
    .set_adv_data = unsupported_ble_host_set_adv_data,
    .start_advertising = unsupported_ble_host_start_advertising,
    .stop_advertising = unsupported_ble_host_stop_advertising,
    .adv_set_create = unsupported_ble_host_adv_set_create,
    .adv_set_set_data = unsupported_ble_host_adv_set_set_data,
    .adv_set_set_encoded_data =
        unsupported_ble_host_adv_set_set_encoded_data,
    .adv_set_set_scan_response_data =
        unsupported_ble_host_adv_set_set_scan_response_data,
    .adv_set_start = unsupported_ble_host_adv_set_operation,
    .adv_set_stop = unsupported_ble_host_adv_set_operation,
    .adv_set_destroy = unsupported_ble_host_adv_set_operation,
    .start_scan = unsupported_ble_host_start_scan,
    .stop_scan = unsupported_ble_host_stop_scan,
    .register_gatt_services = unsupported_ble_host_register_gatt_services,
    .unregister_gatt_services = unsupported_ble_host_unregister_gatt_services,
    .notify = unsupported_ble_host_notify,
    .indicate = unsupported_ble_host_indicate,
    .connect = unsupported_ble_host_connect,
    .disconnect = unsupported_ble_host_disconnect,
    .update_connection = unsupported_ble_host_update_connection,
    .exchange_mtu = unsupported_ble_host_exchange_mtu,
    .set_preferred_phy = unsupported_ble_host_set_preferred_phy,
    .read_phy = unsupported_ble_host_read_phy,
    .gatt_discover = unsupported_ble_host_gatt_discover,
    .gatt_read = unsupported_ble_host_gatt_read,
    .gatt_write = unsupported_ble_host_gatt_write,
    .gatt_subscribe = unsupported_ble_host_gatt_subscribe,
};
static const h2_pal_ble_host_api_t unsupported_ble_host_api = { .user = NULL, .vtable = &unsupported_ble_host_vtable };
const h2_pal_ble_host_api_t *h2_pal_unsupported_ble_host_api(void) { return &unsupported_ble_host_api; }
