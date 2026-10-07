#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/net/h2_pal_netif.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  h2_pal_wifi_sta_status_t status = {.state = H2_PAL_WIFI_STA_STATE_GOT_IP};
  assert(!h2_pal_wifi_sta_status_has_ip(&status));
  status.ip_valid = 1;
  status.ip.ip4 = 0xc0000201;
  assert(h2_pal_wifi_sta_status_has_ip(&status));
  status.ip_valid = 0;
  status.ip.ip4 = 0;
  status.ip.ip6_valid = 1;
  status.ip.ip6[0] = 0xfd;
  status.ip.ip6[15] = 1;
  assert(h2_pal_wifi_sta_status_has_ip(&status));
  h2_pal_netif_status_t netif = {.kind = H2_PAL_NETIF_KIND_WIFI_STA,
                                 .flags = H2_PAL_NETIF_FLAG_UP |
                                          H2_PAL_NETIF_FLAG_LINK_UP |
                                          H2_PAL_NETIF_FLAG_HAS_IPV6,
                                 .ipv6 = {.family = H2_PAL_NET_FAMILY_IPV6}};
  memcpy(netif.ipv6.ip, status.ip.ip6, 16);
  assert(h2_pal_netif_status_is_usable(&netif));
  const uint8_t valid[][16] = {
      {0x20, 1, 0xdb, 0x8}, {0xfd, 0}, {0x00, 0x64, 0xff, 0x9b}};
  const uint8_t invalid[][16] = {
      {0},          {[15] = 1},
      {0xfe, 0x80}, {0xfe, 0xbf},
      {0xff, 2},    {[10] = 0xff, [11] = 0xff, [12] = 192, [15] = 1}};
  for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
    memcpy(status.ip.ip6, valid[i], 16);
    memcpy(netif.ipv6.ip, valid[i], 16);
    assert(h2_pal_wifi_sta_status_has_ip(&status));
    assert(h2_pal_netif_status_is_usable(&netif));
  }
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    memcpy(status.ip.ip6, invalid[i], 16);
    memcpy(netif.ipv6.ip, invalid[i], 16);
    assert(!h2_pal_wifi_sta_status_has_ip(&status));
    assert(!h2_pal_netif_status_is_usable(&netif));
  }
  status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  assert(!h2_pal_wifi_sta_status_has_ip(&status));
  assert(!h2_pal_wifi_sta_status_has_ip(NULL));
  puts("Public Wi-Fi/Netif IPv6 readiness PASS");
  return 0;
}
