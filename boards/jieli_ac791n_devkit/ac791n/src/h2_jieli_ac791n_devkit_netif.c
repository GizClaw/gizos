#include "asm/includes.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2/pal/net/h2_pal_netif.h"

#ifdef H2_JIELI_NETWORK_ENABLE

#include "lwip.h"
#include "lwip/dns.h"
#include "wifi/wifi_connect.h"

#include <string.h>

static h2_pal_netif_kind_t current_kind(void) {
  struct wifi_mode_info mode = {0};
  wifi_get_mode_cur_info(&mode);
  return mode.mode == AP_MODE ? H2_PAL_NETIF_KIND_WIFI_AP
                              : H2_PAL_NETIF_KIND_WIFI_STA;
}

static h2_pal_netif_ref_t wifi_ref(void) {
  h2_pal_netif_ref_t ref;
  memset(&ref, 0, sizeof(ref));
  ref.type = H2_PAL_NETIF_REF_NAME;
  ref.kind = current_kind();
  strcpy(ref.name, "wl0");
  return ref;
}

static int matches_filter(const h2_pal_netif_filter_t *filter) {
  if (filter == NULL) return 1;
  if (filter->kind != H2_PAL_NETIF_KIND_UNKNOWN &&
      filter->kind != current_kind()) {
    return 0;
  }
  if (filter->name != NULL && strcmp(filter->name, "wl0") != 0) return 0;
  if (filter->id != 0u && filter->id != WIFI_NETIF) return 0;
  return 1;
}

static void set_ipv4(h2_pal_net_addr_t *out, const uint8_t bytes[4]) {
  memset(out, 0, sizeof(*out));
  out->family = H2_PAL_NET_FAMILY_IPV4;
  memcpy(out->ip, bytes, 4u);
}

static h2_pal_result_t status_for_wifi(h2_pal_netif_status_t *out_status) {
  if (out_status == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_status, 0, sizeof(*out_status));
  out_status->ref = wifi_ref();
  out_status->kind = out_status->ref.kind;
  out_status->mtu = 1500u;
  if (wifi_get_mac(out_status->mac) == 0) out_status->mac_valid = 1u;
  if (!wifi_is_on()) return H2_PAL_OK;
  out_status->flags = H2_PAL_NETIF_FLAG_UP | H2_PAL_NETIF_FLAG_LINK_UP;
  struct lan_setting *lan = net_get_lan_info(WIFI_NETIF);
  if (lan != NULL) {
    const uint8_t ip[4] = {
        lan->WIRELESS_IP_ADDR0, lan->WIRELESS_IP_ADDR1,
        lan->WIRELESS_IP_ADDR2, lan->WIRELESS_IP_ADDR3,
    };
    const uint8_t mask[4] = {
        lan->WIRELESS_NETMASK0, lan->WIRELESS_NETMASK1,
        lan->WIRELESS_NETMASK2, lan->WIRELESS_NETMASK3,
    };
    const uint8_t gateway[4] = {
        lan->WIRELESS_GATEWAY0, lan->WIRELESS_GATEWAY1,
        lan->WIRELESS_GATEWAY2, lan->WIRELESS_GATEWAY3,
    };
    set_ipv4(&out_status->ipv4, ip);
    set_ipv4(&out_status->netmask4, mask);
    set_ipv4(&out_status->gateway4, gateway);
    if (ip[0] != 0u || ip[1] != 0u || ip[2] != 0u || ip[3] != 0u) {
      out_status->flags |=
          H2_PAL_NETIF_FLAG_HAS_IPV4 | H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
    }
  }
  for (size_t index = 0u;
       index < H2_PAL_NETIF_DNS_MAX && index < DNS_MAX_SERVERS; ++index) {
    const ip_addr_t *server = dns_getserver((u8_t)index);
    if (server == NULL || !IP_IS_V4(server) || ip_addr_isany(server)) continue;
    out_status->dns[out_status->dns_count].addr.family =
        H2_PAL_NET_FAMILY_IPV4;
    memcpy(
        out_status->dns[out_status->dns_count].addr.ip,
        &ip_2_ip4(server)->addr, 4u);
    ++out_status->dns_count;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_jieli_netif_list(
    void *user, const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn on_netif, void *callback_user) {
  (void)user;
  if (on_netif == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!matches_filter(filter)) return H2_PAL_OK;
  h2_pal_netif_status_t status;
  h2_pal_result_t result = status_for_wifi(&status);
  if (result != H2_PAL_OK) return result;
  return on_netif(callback_user, &status.ref, &status);
}

static h2_pal_result_t h2_jieli_netif_find(
    void *user, const h2_pal_netif_filter_t *filter,
    h2_pal_netif_ref_t *out_ref) {
  (void)user;
  if (filter == NULL || out_ref == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!matches_filter(filter)) return H2_PAL_ERR_NOT_FOUND;
  *out_ref = wifi_ref();
  return H2_PAL_OK;
}

static h2_pal_result_t h2_jieli_netif_get_status(
    void *user, const h2_pal_netif_ref_t *ref,
    h2_pal_netif_status_t *out_status) {
  (void)user;
  if (ref != NULL && !h2_pal_netif_ref_is_default(ref) &&
      !(ref->type == H2_PAL_NETIF_REF_NAME &&
        strcmp(ref->name, "wl0") == 0)) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  return status_for_wifi(out_status);
}

static h2_pal_result_t h2_jieli_netif_get_dns(
    void *user, const h2_pal_netif_ref_t *ref,
    h2_pal_netif_dns_server_t *out_servers, size_t max_servers,
    size_t *out_count) {
  (void)user;
  if (out_count == NULL || (out_servers == NULL && max_servers != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_netif_status_t status;
  h2_pal_result_t result = h2_jieli_netif_get_status(NULL, ref, &status);
  if (result != H2_PAL_OK) return result;
  *out_count = status.dns_count < max_servers ? status.dns_count : max_servers;
  memcpy(out_servers, status.dns, *out_count * sizeof(*out_servers));
  return H2_PAL_OK;
}

const h2_pal_netif_api_t *h2_jieli_ac791n_devkit_netif_api(void) {
  static const h2_pal_netif_vtable_t vtable = {
      .list = h2_jieli_netif_list,
      .find = h2_jieli_netif_find,
      .get_status = h2_jieli_netif_get_status,
      .get_dns_servers = h2_jieli_netif_get_dns,
  };
  static const h2_pal_netif_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}

#else

const h2_pal_netif_api_t *h2_jieli_ac791n_devkit_netif_api(void) {
  return NULL;
}

#endif
