#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_wifi_ap_start(void *p0, const h2_pal_wifi_ap_config_t *p1, uint32_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_ap_stop(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_ap_get_status(void *p0, h2_pal_wifi_ap_status_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_ap_get_clients(void *p0, h2_pal_wifi_ap_client_t *p1, size_t p2, size_t *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_wifi_ap_get_mac(void *p0, uint8_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_wifi_ap_vtable_t unsupported_wifi_ap_vtable = {
    .start = unsupported_wifi_ap_start,
    .stop = unsupported_wifi_ap_stop,
    .get_status = unsupported_wifi_ap_get_status,
    .get_clients = unsupported_wifi_ap_get_clients,
    .get_mac = unsupported_wifi_ap_get_mac,
};
static const h2_pal_wifi_ap_api_t unsupported_wifi_ap_api = { .user = NULL, .vtable = &unsupported_wifi_ap_vtable };
const h2_pal_wifi_ap_api_t *h2_pal_unsupported_wifi_ap_api(void) { return &unsupported_wifi_ap_api; }
