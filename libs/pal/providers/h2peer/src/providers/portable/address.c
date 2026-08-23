#include <stdio.h>
#include <string.h>

#include "address.h"

int h2_peer_net_addr_family_is_valid(h2_pal_net_family_t family) {
  return family == H2_PAL_NET_FAMILY_IPV4 ||
         family == H2_PAL_NET_FAMILY_IPV6;
}

int h2_peer_net_addr_format(
    const h2_pal_net_addr_t* addr, char* buf, size_t len) {
  if (addr == NULL || buf == NULL || len == 0u) {
    return 0;
  }
  memset(buf, 0, len);
  switch (addr->family) {
    case H2_PAL_NET_FAMILY_IPV6: {
      const uint8_t* ip = addr->ip;
      int written = snprintf(
          buf, len, "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
          ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7],
          ip[8], ip[9], ip[10], ip[11], ip[12], ip[13], ip[14], ip[15]);
      if (written > 0 && (size_t)written < len) {
        return 1;
      }
      buf[0] = '\0';
      return 0;
    }
    case H2_PAL_NET_FAMILY_IPV4: {
      const uint8_t* ip = addr->ip;
      int written = snprintf(buf, len, "%u.%u.%u.%u", ip[0], ip[1],
                             ip[2], ip[3]);
      if (written > 0 && (size_t)written < len) {
        return 1;
      }
      buf[0] = '\0';
      return 0;
    }
    default:
      return 0;
  }
}
