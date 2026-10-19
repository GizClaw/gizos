#include "h2_bk_net_addr.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  assert(h2_bk_net_family(H2_PAL_NET_FAMILY_IPV6) < 0);
  h2_pal_net_addr_t addr = {.family = H2_PAL_NET_FAMILY_IPV6, .ip = {0xfd}};
  struct sockaddr_storage native;
  socklen_t size = 0;
  assert(h2_bk_net_to_sockaddr(&addr, &native, &size) ==
         H2_PAL_ERR_UNSUPPORTED);
  h2_pal_net_addr_list_t list;
  assert(h2_bk_net_resolve_all(NULL, "::1", H2_PAL_NET_FAMILY_IPV6, &list) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(list.count == 0);
  assert(h2_bk_net_resolve_all(NULL, "localhost", H2_PAL_NET_FAMILY_ANY,
                               &list) == H2_PAL_OK);
  assert(list.count == 1 && list.addrs[0].family == H2_PAL_NET_FAMILY_IPV4);
  puts("BK production IPv6-disabled compilation/unsupported PASS");
  return 0;
}
