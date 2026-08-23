#include "ports.h"

#include <string.h>

int ports_get_host_addr(const h2_pal_net_api_t* net, h2_pal_net_addr_t* addr,
                        const char* iface_prefix) {
  if (addr == NULL ||
      !h2_peer_net_addr_family_is_valid(addr->family)) {
    return 0;
  }
  h2_pal_net_addr_t host;
  h2_pal_net_family_t requested = addr->family;
  memset(&host, 0, sizeof(host));
  if (h2_pal_net_get_host_addr(net, iface_prefix, &host) != H2_PAL_OK ||
      host.family != requested) {
    return 0;
  }
  uint16_t port = addr->port;
  *addr = host;
  addr->port = port;
  return 1;
}

int ports_resolve_addr(const h2_pal_net_api_t* net, const char* host,
                       h2_pal_net_addr_t* addr) {
  if (addr == NULL) {
    return -1;
  }
  memset(addr, 0, sizeof(*addr));
  if (h2_pal_net_resolve_addr(net, host, addr) != H2_PAL_OK ||
      !h2_peer_net_addr_family_is_valid(addr->family)) {
    memset(addr, 0, sizeof(*addr));
    return -1;
  }
  return 0;
}

void ports_sleep_ms(const h2_pal_time_api_t* time, int ms) {
  if (ms > 0) {
    (void)h2_pal_time_sleep_ms(time, (uint32_t)ms);
  }
}
