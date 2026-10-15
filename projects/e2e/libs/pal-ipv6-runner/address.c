#include "h2_pal_ipv6_runner.h"
int h2_ipv6_parse_address(const h2_pal_net_api_t *net, const char *text,
                          h2_pal_net_addr_t *out) {
  h2_pal_net_addr_list_t list;
  int rc = h2_pal_net_resolve_all(net, text, H2_PAL_NET_FAMILY_IPV6, &list);
  if (rc == H2_PAL_OK) {
    if (!list.count)
      return H2_PAL_ERR_FORMAT;
    *out = list.addrs[0];
  }
  return rc;
}
