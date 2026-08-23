#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_wifi_sta_get_status(void *p0, h2_pal_wifi_sta_status_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_sta_scan(void *p0, const h2_pal_wifi_scan_request_t *p1, h2_pal_wifi_scan_result_fn p2, void *p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_sta_connect(void *p0, const h2_pal_wifi_sta_config_t *p1, uint32_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_sta_disconnect(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_sta_get_mac(void *p0, uint8_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_wifi_sta_vtable_t unsupported_wifi_sta_vtable = {
    .get_status = unsupported_wifi_sta_get_status,
    .scan = unsupported_wifi_sta_scan,
    .connect = unsupported_wifi_sta_connect,
    .disconnect = unsupported_wifi_sta_disconnect,
    .get_mac = unsupported_wifi_sta_get_mac,
};
static const h2_pal_wifi_sta_api_t unsupported_wifi_sta_api = { .user = NULL, .vtable = &unsupported_wifi_sta_vtable };
const h2_pal_wifi_sta_api_t *h2_pal_unsupported_wifi_sta_api(void) { return &unsupported_wifi_sta_api; }
