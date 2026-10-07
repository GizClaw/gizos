#include "h2_esp_wifi_ip.h"
#include "esp_netif_net_stack.h"

#include "lwip/def.h"
#include "lwip/nd6.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "sdkconfig.h"
#include <string.h>

#if CONFIG_LWIP_IPV6
static void h2_esp_wifi_ip_clear(void *raw) {
  struct netif *netif = raw;
  if (netif == NULL)
    return;
  netif_set_ip6_autoconfig_enabled(netif, 0);
  nd6_cleanup_netif(netif);
  for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
    netif_ip6_addr_set_state(netif, i, IP6_ADDR_INVALID);
    netif_ip6_addr_set(netif, i, IP6_ADDR_ANY6);
  }
}

#if CONFIG_LWIP_IPV6_AUTOCONFIG
static void h2_esp_wifi_ip_initialize(void *raw) {
  h2_esp_wifi_ip_clear(raw);
  struct netif *netif = raw;
  netif_set_ip6_autoconfig_enabled(netif, 1);
  netif_create_ip6_linklocal_address(netif, 1);
}
#endif
#endif

int h2_esp_wifi_ip_start(esp_netif_t *netif) {
  if (netif == NULL)
    return H2_PAL_ERR_INVALID_ARG;
#if CONFIG_LWIP_IPV6 && CONFIG_LWIP_IPV6_AUTOCONFIG
  void *native = esp_netif_get_netif_impl(netif);
  if (native == NULL || !esp_netif_is_netif_up(netif))
    return H2_PAL_ERR_UNAVAILABLE;
  return tcpip_callback_wait(h2_esp_wifi_ip_initialize, native) == ERR_OK
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
#else
  return H2_PAL_ERR_UNSUPPORTED;
#endif
}

void h2_esp_wifi_ip_stop(esp_netif_t *netif) {
#if CONFIG_LWIP_IPV6
  if (netif != NULL)
    (void)tcpip_callback_wait(h2_esp_wifi_ip_clear,
                              esp_netif_get_netif_impl(netif));
#else
  (void)netif;
#endif
}

void h2_esp_wifi_ip_snapshot(esp_netif_t *netif,
                             h2_pal_wifi_sta_status_t *status) {
  memset(&status->ip, 0, sizeof(status->ip));
  status->ip_valid = 0u;
  if (netif == NULL || !esp_netif_is_netif_up(netif))
    return;
  esp_netif_ip_info_t ip4 = {0};
  if (esp_netif_get_ip_info(netif, &ip4) == ESP_OK && ip4.ip.addr != 0u) {
    status->ip.ip4 = lwip_ntohl(ip4.ip.addr);
    status->ip.netmask4 = lwip_ntohl(ip4.netmask.addr);
    status->ip.gateway4 = lwip_ntohl(ip4.gw.addr);
    status->ip_valid = 1u;
  }
#if CONFIG_LWIP_IPV6
  esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
  int count = esp_netif_get_all_preferred_ip6(netif, addresses);
  for (int i = 0; i < count; ++i) {
    const uint8_t *ip = (const uint8_t *)addresses[i].addr;
    if (!h2_pal_net_ipv6_is_non_link_local_unicast(ip))
      continue;
    memcpy(status->ip.ip6, ip, sizeof(status->ip.ip6));
    status->ip.ip6_valid = 1u;
    break;
  }
#endif
}
