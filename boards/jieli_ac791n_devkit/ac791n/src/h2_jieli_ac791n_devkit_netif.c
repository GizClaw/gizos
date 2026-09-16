#include "asm/includes.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_network.h"
#include "h2/pal/net/h2_pal_netif.h"

#ifdef H2_JIELI_NETWORK_ENABLE

#include "lwip.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "wifi/wifi_connect.h"

#include <string.h>

static int matches_filter(
    const h2_pal_netif_filter_t *filter, h2_pal_netif_kind_t kind) {
  if (filter == NULL) return 1;
  if (filter->kind != H2_PAL_NETIF_KIND_UNKNOWN && filter->kind != kind) return 0;
  if (filter->name != NULL && strcmp(filter->name, "wl0") != 0) return 0;
  if (filter->id != 0u && filter->id != WIFI_NETIF) return 0;
  return 1;
}

static void set_ipv4(h2_pal_net_addr_t *out, uint32_t address) {
  memset(out, 0, sizeof(*out));
  out->family = H2_PAL_NET_FAMILY_IPV4;
  memcpy(out->ip, &address, 4u);
}

static void capture_ip_on_tcpip(void *context) {
  h2_pal_netif_status_t *status = context;
  struct netif_info info = {0};
  lwip_get_netif_info(WIFI_NETIF, &info);
  set_ipv4(&status->ipv4, info.ip);
  set_ipv4(&status->netmask4, info.netmask);
  set_ipv4(&status->gateway4, info.gw);
  status->flags &= ~(H2_PAL_NETIF_FLAG_HAS_IPV4 | H2_PAL_NETIF_FLAG_DEFAULT_ROUTE);
  if (info.ip != 0u) {
    status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV4 | H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
  }
  status->dns_count = 0u;
  for (size_t index = 0u;
       index < H2_PAL_NETIF_DNS_MAX && index < DNS_MAX_SERVERS; ++index) {
    const ip_addr_t *server = dns_getserver((u8_t)index);
    if (server == NULL || !IP_IS_V4(server) || ip_addr_isany(server)) continue;
    set_ipv4(&status->dns[status->dns_count].addr, ip_2_ip4(server)->addr);
    ++status->dns_count;
  }
}

int h2_jieli_netif_capture_ip(h2_pal_netif_status_t *status) {
  /* callback_wait waits for completion, unlike callback_with_block's queue
   * admission flag. Never enqueue and wait on the TCP/IP thread itself. */
  const char *task = os_current_task();
  if (task != NULL && strcmp(task, "tcpip_thread") == 0) {
    capture_ip_on_tcpip(status);
    return H2_PAL_OK;
  }
  return tcpip_callback_wait(capture_ip_on_tcpip, status) == ERR_OK
      ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t status_for_wifi(h2_pal_netif_status_t *out_status) {
  if (out_status == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_netif_status_t status;
  uint32_t generation;
  int result = h2_jieli_wifi_netif_begin(&status, &generation);
  if (result != H2_PAL_OK) return result;
  if ((status.flags & H2_PAL_NETIF_FLAG_UP) != 0u) {
    if (wifi_get_mac(status.mac) == 0) status.mac_valid = 1u;
    if ((status.flags & H2_PAL_NETIF_FLAG_HAS_IPV4) != 0u) {
      result = h2_jieli_netif_capture_ip(&status);
    }
  }
  const int end_result = h2_jieli_wifi_netif_end(generation);
  if (result != H2_PAL_OK) return result;
  if (end_result != H2_PAL_OK) return end_result;
  *out_status = status;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_jieli_netif_list(
    void *user, const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn on_netif, void *callback_user) {
  (void)user;
  if (on_netif == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_netif_status_t status;
  h2_pal_result_t result = status_for_wifi(&status);
  if (result != H2_PAL_OK) return result;
  if (!matches_filter(filter, status.kind)) return H2_PAL_OK;
  return on_netif(callback_user, &status.ref, &status);
}

static h2_pal_result_t h2_jieli_netif_find(
    void *user, const h2_pal_netif_filter_t *filter,
    h2_pal_netif_ref_t *out_ref) {
  (void)user;
  if (filter == NULL || out_ref == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_netif_status_t status;
  int result = status_for_wifi(&status);
  if (result != H2_PAL_OK) return result;
  if (!matches_filter(filter, status.kind)) return H2_PAL_ERR_NOT_FOUND;
  *out_ref = status.ref;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_jieli_netif_get_status(
    void *user, const h2_pal_netif_ref_t *ref,
    h2_pal_netif_status_t *out_status) {
  (void)user;
  if (ref != NULL) {
    if (!h2_pal_netif_kind_is_valid(ref->kind) ||
        (ref->type != H2_PAL_NETIF_REF_DEFAULT &&
         ref->type != H2_PAL_NETIF_REF_NAME &&
         ref->type != H2_PAL_NETIF_REF_ID)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_pal_netif_ref_is_default(ref)) {
      if (ref->type != H2_PAL_NETIF_REF_NAME) return H2_PAL_ERR_NOT_FOUND;
      size_t length = strnlen(ref->name, sizeof(ref->name));
      if (length == sizeof(ref->name)) return H2_PAL_ERR_INVALID_ARG;
      if (length != 3u || memcmp(ref->name, "wl0", 3u) != 0) {
        return H2_PAL_ERR_NOT_FOUND;
      }
    }
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
  if (*out_count != 0u) {
    memcpy(out_servers, status.dns, *out_count * sizeof(*out_servers));
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_jieli_netif_set_default(
    void *user, const h2_pal_netif_ref_t *ref) {
  (void)user;
  (void)ref;
  return H2_PAL_ERR_UNSUPPORTED;
}

const h2_pal_netif_api_t *h2_jieli_ac791n_devkit_netif_api(void) {
  static const h2_pal_netif_vtable_t vtable = {
      .list = h2_jieli_netif_list,
      .find = h2_jieli_netif_find,
      .get_status = h2_jieli_netif_get_status,
      .get_dns_servers = h2_jieli_netif_get_dns,
      .set_default = h2_jieli_netif_set_default,
  };
  static const h2_pal_netif_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}

#else

const h2_pal_netif_api_t *h2_jieli_ac791n_devkit_netif_api(void) {
  return NULL;
}

#endif
