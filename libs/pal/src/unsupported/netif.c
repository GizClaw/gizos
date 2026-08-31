#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_netif_list(void *p0, const h2_pal_netif_filter_t *p1, h2_pal_netif_list_fn p2, void *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_netif_find(void *p0, const h2_pal_netif_filter_t *p1, h2_pal_netif_ref_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_netif_get_status(void *p0, const h2_pal_netif_ref_t *p1, h2_pal_netif_status_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_netif_get_dns_servers(void *p0, const h2_pal_netif_ref_t *p1, h2_pal_netif_dns_server_t *p2, size_t p3, size_t *p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_netif_set_default(void *p0, const h2_pal_netif_ref_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_netif_vtable_t unsupported_netif_vtable = {
    .list = unsupported_netif_list,
    .find = unsupported_netif_find,
    .get_status = unsupported_netif_get_status,
    .get_dns_servers = unsupported_netif_get_dns_servers,
    .set_default = unsupported_netif_set_default,
};
static const h2_pal_netif_api_t unsupported_netif_api = { .user = NULL, .vtable = &unsupported_netif_vtable };
const h2_pal_netif_api_t *h2_pal_unsupported_netif_api(void) { return &unsupported_netif_api; }
