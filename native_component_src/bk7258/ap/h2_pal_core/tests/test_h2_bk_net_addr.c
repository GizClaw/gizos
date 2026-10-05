#include "h2_bk_net_addr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  h2_pal_net_addr_t addr = {
      .family = H2_PAL_NET_FAMILY_IPV6,
      .port = 5201,
      .ip = {0xfd, 0x53, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}};
  struct sockaddr_storage storage;
  socklen_t size = 0;
  assert(h2_bk_net_to_sockaddr(&addr, &storage, &size) == H2_PAL_OK);
  assert(size == sizeof(struct sockaddr_in6));
  h2_pal_net_addr_t got;
  assert(h2_bk_net_from_sockaddr((struct sockaddr *)&storage, &got) ==
         H2_PAL_OK);
  assert(!memcmp(&got, &addr, sizeof(got)));
  addr.ip[0] = 0xfe;
  addr.ip[1] = 0x80;
  assert(h2_bk_net_to_sockaddr(&addr, &storage, &size) ==
         H2_PAL_ERR_INVALID_ARG);
  addr.scope_id = 2;
  assert(h2_bk_net_to_sockaddr(&addr, &storage, &size) == H2_PAL_OK);
  assert(h2_bk_net_from_sockaddr((struct sockaddr *)&storage, &got) ==
         H2_PAL_OK);
  assert(got.scope_id == 2 && !memcmp(got.ip, addr.ip, 16));
  addr.scope_id = 256;
  assert(h2_bk_net_to_sockaddr(&addr, &storage, &size) ==
         H2_PAL_ERR_INVALID_ARG);
  addr = (h2_pal_net_addr_t){
      .family = H2_PAL_NET_FAMILY_IPV4, .ip = {192, 0, 2, 1}, .port = 5201};
  assert(h2_bk_net_to_sockaddr(&addr, &storage, &size) == H2_PAL_OK);
  assert(h2_bk_net_from_sockaddr((struct sockaddr *)&storage, &got) ==
         H2_PAL_OK);
  assert(!memcmp(&addr, &got, sizeof(addr)));
  h2_pal_net_addr_list_t list;
  assert(h2_bk_net_resolve_all(NULL, "fd53::1", H2_PAL_NET_FAMILY_IPV6,
                               &list) == H2_PAL_OK);
  assert(list.count == 1 && list.addrs[0].family == H2_PAL_NET_FAMILY_IPV6);
  assert(h2_bk_net_resolve_all(NULL, "fe80::1%2", H2_PAL_NET_FAMILY_IPV6,
                               &list) == H2_PAL_OK);
  assert(list.addrs[0].scope_id == 2);
  assert(h2_bk_net_resolve_all(NULL, "fe80::1%0", H2_PAL_NET_FAMILY_ANY,
                               &list) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_net_resolve_all(NULL, "no-such.invalid", H2_PAL_NET_FAMILY_ANY,
                               &list) == H2_PAL_ERR_NOT_FOUND);
  assert(list.count == 0);
  assert(h2_bk_net_resolve_all(NULL, "localhost.", H2_PAL_NET_FAMILY_ANY,
                               &list) == H2_PAL_OK);
  assert(list.count == 2 && list.addrs[0].family == H2_PAL_NET_FAMILY_IPV6 &&
         h2_bk_net_first_addr(&list).family == H2_PAL_NET_FAMILY_IPV4);
  assert(h2_bk_net_resolve_all(NULL, "localhost", H2_PAL_NET_FAMILY_IPV6,
                               &list) == H2_PAL_OK);
  assert(list.count == 1 && list.addrs[0].ip[15] == 1);
  assert(h2_bk_net_resolve_all(NULL, "192.0.2.1", H2_PAL_NET_FAMILY_IPV6,
                               &list) == H2_PAL_ERR_NOT_FOUND);
  assert(list.count == 0);
  puts("BK IPv6 production sockaddr and resolver PASS");
  return 0;
}
