#include "h2_bk_wifi_ipv6_sync.h"
#include "lwip/nd6.h"
#include <string.h>

void h2_bk_wifi_ipv6_clear(struct netif *sta) {
#if LWIP_IPV6
  netif_set_ip6_autoconfig_enabled(sta, 0);
  nd6_cleanup_netif(sta);
  for (unsigned i = 0u; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
    netif_ip6_addr_set_state(sta, i, IP6_ADDR_INVALID);
    netif_ip6_addr_set(sta, i, IP6_ADDR_ANY6);
  }
#else
  (void)sta;
#endif
}

err_t h2_bk_wifi_ipv6_install(struct netif *sta,
                              const h2_bk_wifi_ipv6_snapshot_t *snapshot,
                              h2_pal_wifi_sta_status_t *status,
                              uint32_t *cp_generation) {
#if LWIP_IPV6
  memset(&status->ip, 0, sizeof(status->ip));
  status->ip_valid = 0u;
  if (!snapshot->connected) {
    h2_bk_wifi_ipv6_clear(sta);
    ip4_addr_t zero;
    ip4_addr_set_zero(&zero);
    netif_set_link_down(sta);
    netif_set_down(sta);
    netif_set_addr(sta, &zero, &zero, &zero);
    if (netif_default == sta)
      netif_set_default(NULL);
    return ERR_OK;
  }
  /* CP owns SLAAC and DAD for the radio. AP owns the socket stack and
   * installs only this coherent, current CP preferred-address snapshot. */
  netif_set_ip6_autoconfig_enabled(sta, 0);
  if (*cp_generation != snapshot->generation) {
    nd6_cleanup_netif(sta);
    for (unsigned i = 0u; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
      netif_ip6_addr_set_state(sta, i, IP6_ADDR_INVALID);
      netif_ip6_addr_set(sta, i, IP6_ADDR_ANY6);
    }
    *cp_generation = snapshot->generation;
  }
  unsigned next = 1u;
  uint8_t used[LWIP_IPV6_NUM_ADDRESSES] = {0};
  for (uint32_t i = 0u; i < snapshot->count; ++i) {
    ip6_addr_t address = {0};
    memcpy(address.addr, snapshot->addresses[i].address, 16u);
    const unsigned slot = ip6_addr_islinklocal(&address) ? 0u : next++;
    if (slot >= LWIP_IPV6_NUM_ADDRESSES)
      return ERR_BUF;
    used[slot] = 1u;
    ip6_addr_assign_zone(&address, IP6_UNICAST, sta);
    if (memcmp(netif_ip6_addr(sta, slot)->addr, address.addr, 16u) != 0)
      netif_ip6_addr_set(sta, slot, &address);
    netif_ip6_addr_set_state(sta, slot, IP6_ADDR_PREFERRED);
    if (!status->ip.ip6_valid && h2_pal_net_ipv6_is_non_link_local_unicast(
                                     (const uint8_t *)address.addr)) {
      memcpy(status->ip.ip6, address.addr, 16u);
      status->ip.ip6_valid = 1u;
    }
  }
  for (unsigned i = 0u; i < LWIP_IPV6_NUM_ADDRESSES; ++i)
    if (!used[i]) {
      netif_ip6_addr_set_state(sta, i, IP6_ADDR_INVALID);
      netif_ip6_addr_set(sta, i, IP6_ADDR_ANY6);
    }
  netif_set_link_up(sta);
  netif_set_up(sta);
  if (netif_default == NULL)
    netif_set_default(sta);
  const uint32_t ip4 = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(sta)));
  if (ip4 != 0u) {
    status->ip.ip4 = ip4;
    status->ip.netmask4 = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(sta)));
    status->ip.gateway4 = lwip_ntohl(ip4_addr_get_u32(netif_ip4_gw(sta)));
    status->ip_valid = 1u;
  }
  return ERR_OK;
#else
  (void)sta;
  (void)snapshot;
  (void)status;
  (void)cp_generation;
  return ERR_IF;
#endif
}
