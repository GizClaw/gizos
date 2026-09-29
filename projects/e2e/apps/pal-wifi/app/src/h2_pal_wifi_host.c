#include "h2_pal_wifi_e2e.h"

#include <stdio.h>
#include <string.h>

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "H2_WIFI_HOST_FAILURE line=%u\n", __LINE__);                           \
            return H2_PAL_ERR_IO;                                                                  \
        }                                                                                          \
    } while (0)
static bool scan(void *user, const h2_pal_wifi_scan_entry_t *entry) {
    (void)user;
    (void)entry;
    return true;
}
typedef struct host_list {
    unsigned count;
    int bad, stop;
    h2_pal_netif_ref_t first;
} host_list_t;
static int list(void *user, const h2_pal_netif_ref_t *ref, const h2_pal_netif_status_t *status) {
    host_list_t *o = user;
    ++o->count;
    if (!h2_pal_netif_ref_is_concrete(ref) || !status ||
        !h2_pal_netif_ref_equal(ref, &status->ref) || status->dns_count > 2)
        o->bad = 1;
    if (o->count == 1 && ref)
        o->first = *ref;
    return o->stop;
}
int h2_wifi_host_contract(const h2_pal_wifi_sta_api_t *sta, const h2_pal_wifi_ap_api_t *ap,
                          const h2_pal_wifi_settings_api_t *settings,
                          const h2_pal_netif_api_t *netif, int host_kind, unsigned *out_netifs) {
    if (!out_netifs)
        return H2_PAL_ERR_INVALID_ARG;
    *out_netifs = 0;
    h2_pal_wifi_sta_config_t cfg = {.ssid = "fixture", .ssid_len = 7};
    h2_pal_wifi_ap_config_t ac = {
        .ssid = "fixture", .ssid_len = 7, .security = H2_PAL_WIFI_SECURITY_OPEN, .max_clients = 1};
    h2_pal_wifi_sta_status_t ss;
    h2_pal_wifi_ap_status_t as;
    uint8_t mac[6];
    size_t count = 99;
    int has = 99;
    REQUIRE(h2_pal_wifi_sta_get_status(sta, &ss) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_sta_scan(sta, NULL, scan, NULL, 10) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_sta_connect(sta, &cfg, 10) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_sta_connect_and_save(sta, &cfg, 10) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_sta_disconnect(sta) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_sta_get_mac(sta, mac) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_sta_set_power_save(sta, H2_PAL_WIFI_POWER_SAVE_NONE) ==
            H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_ap_start(ap, &ac, 10) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_ap_stop(ap, 10) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_ap_get_status(ap, &as) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_ap_get_clients(ap, NULL, 0, &count) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_ap_get_mac(ap, mac) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_settings_get_saved_sta_config(settings, &cfg) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_settings_set_saved_sta_config(settings, &cfg) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_settings_clear_saved_sta_config(settings) == H2_PAL_ERR_UNSUPPORTED);
    REQUIRE(h2_pal_wifi_settings_has_saved_sta_config(settings, &has) == H2_PAL_ERR_UNSUPPORTED);
    host_list_t o = {0};
    h2_pal_netif_filter_t any = {0};
    h2_pal_netif_ref_t ref;
    h2_pal_netif_status_t status;
    int rc = h2_pal_netif_list(netif, NULL, list, &o);
    if (host_kind < 0) {
        REQUIRE(rc == H2_PAL_ERR_UNSUPPORTED);
        REQUIRE(h2_pal_netif_find(netif, &any, &ref) == H2_PAL_ERR_UNSUPPORTED);
        REQUIRE(h2_pal_netif_get_status(netif, NULL, &status) == H2_PAL_ERR_UNSUPPORTED);
        REQUIRE(h2_pal_netif_get_dns_servers(netif, NULL, NULL, 0, &count) ==
                H2_PAL_ERR_UNSUPPORTED);
        ref = (h2_pal_netif_ref_t){.type = H2_PAL_NETIF_REF_ID, .id = 0xfffffffeu};
        REQUIRE(h2_pal_netif_set_default(netif, &ref) == H2_PAL_ERR_UNSUPPORTED);
        return H2_PAL_OK;
    }
    REQUIRE(rc == H2_PAL_OK && o.count && !o.bad);
    *out_netifs = o.count;
    REQUIRE(h2_pal_netif_find(netif, &any, &ref) == H2_PAL_OK);
    REQUIRE(h2_pal_netif_get_status(netif, &ref, &status) == H2_PAL_OK);
    REQUIRE(h2_pal_netif_ref_equal(&ref, &status.ref));
    o = (host_list_t){.stop = 1};
    REQUIRE(h2_pal_netif_list(netif, NULL, list, &o) == H2_PAL_EXIT && o.count == 1);
    h2_pal_netif_ref_t missing = {.type = H2_PAL_NETIF_REF_NAME, .name = "h2wifi-absent"};
    REQUIRE(h2_pal_netif_get_status(netif, &missing, &status) == H2_PAL_ERR_NOT_FOUND);
    REQUIRE(h2_pal_netif_set_default(netif, &missing) ==
            (netif->vtable->set_default ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_UNSUPPORTED));
    h2_pal_netif_filter_t absent = {.name = missing.name};
    REQUIRE(h2_pal_netif_find(netif, &absent, &ref) == H2_PAL_ERR_NOT_FOUND);
    REQUIRE(h2_pal_netif_get_status(netif, &o.first, &status) == H2_PAL_OK);
    h2_pal_netif_dns_server_t dns[2];
    count = 99;
    rc = h2_pal_netif_get_dns_servers(netif, &o.first, dns, 2, &count);
    if (host_kind == H2_PAL_NETIF_KIND_HOST) {
        REQUIRE(status.kind == H2_PAL_NETIF_KIND_HOST && rc == H2_PAL_ERR_UNSUPPORTED &&
                count == 0);
        h2_pal_net_addr_t zero = {0};
        uint8_t zero_mac[6] = {0};
        REQUIRE(!(status.flags & (H2_PAL_NETIF_FLAG_HAS_IPV4 | H2_PAL_NETIF_FLAG_HAS_IPV6)) &&
                !status.dns_count && !status.mtu && !status.mac_valid);
        REQUIRE(!memcmp(&status.ipv4, &zero, sizeof(zero)) &&
                !memcmp(&status.ipv6, &zero, sizeof(zero)) && !memcmp(status.mac, zero_mac, 6));
        REQUIRE(h2_pal_netif_set_default(netif, &status.ref) == H2_PAL_ERR_UNSUPPORTED);
    } else {
        REQUIRE(rc == H2_PAL_OK && count == status.dns_count && count <= 2);
        for (size_t i = 0; i < count; ++i)
            REQUIRE(!memcmp(&dns[i].addr, &status.dns[i].addr, sizeof(dns[i].addr)));
    }
    return H2_PAL_OK;
}
