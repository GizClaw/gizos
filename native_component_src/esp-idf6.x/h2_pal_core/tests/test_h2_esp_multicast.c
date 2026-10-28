#include "h2_esp_platform_net_socket.h"
#include <assert.h>
#include <netinet/in.h>
#include <string.h>

static unsigned calls;
static int option_result;
static h2_pal_net_addr_t expected;
int h2_test_setsockopt(int fd, int level, int option, const void* value,
                       socklen_t length) {
  ++calls;
  assert(fd == 7);
  if (expected.family == H2_PAL_NET_FAMILY_IPV4) {
    assert(level == IPPROTO_IP && option == IP_ADD_MEMBERSHIP);
    assert(length == sizeof(struct ip_mreq));
    const struct ip_mreq* request = value;
    assert(memcmp(&request->imr_multiaddr, expected.ip, 4u) == 0);
    assert(request->imr_interface.s_addr == INADDR_ANY);
  } else {
    assert(level == IPPROTO_IPV6 && option == IPV6_JOIN_GROUP);
    assert(length == sizeof(struct ipv6_mreq));
    const struct ipv6_mreq* request = value;
    assert(memcmp(&request->ipv6mr_multiaddr, expected.ip, 16u) == 0);
    assert(request->ipv6mr_interface == expected.scope_id);
  }
  return option_result;
}

int main(void) {
  expected.family = H2_PAL_NET_FAMILY_IPV4;
  expected.ip[0] = 239u;
  expected.ip[3] = 77u;
  assert(h2_esp_net_join_multicast(-1, &expected) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_esp_net_join_multicast(7, NULL) == H2_PAL_ERR_INVALID_ARG);
  h2_pal_net_addr_t invalid = expected;
  invalid.ip[0] = 223u;
  assert(h2_esp_net_join_multicast(7, &invalid) == H2_PAL_ERR_INVALID_ARG);
  invalid.ip[0] = 240u;
  assert(h2_esp_net_join_multicast(7, &invalid) == H2_PAL_ERR_INVALID_ARG);
  invalid.family = (h2_pal_net_family_t)99;
  assert(h2_esp_net_join_multicast(7, &invalid) == H2_PAL_ERR_UNSUPPORTED);
  assert(calls == 0u);
  assert(h2_esp_net_join_multicast(7, &expected) == H2_PAL_OK);
  option_result = -1;
  assert(h2_esp_net_join_multicast(7, &expected) == H2_PAL_ERR_IO);
  assert(calls == 2u);

  memset(&expected, 0, sizeof(expected));
  expected.family = H2_PAL_NET_FAMILY_IPV6;
  expected.ip[0] = 0xffu;
  expected.ip[1] = 2u;
  expected.ip[15] = 1u;
  assert(h2_esp_net_join_multicast(7, &expected) == H2_PAL_ERR_INVALID_ARG);
  expected.scope_id = 256u;
  assert(h2_esp_net_join_multicast(7, &expected) == H2_PAL_ERR_INVALID_ARG);
  expected.scope_id = 1u;
  invalid = expected;
  invalid.ip[0] = 0xfeu;
  assert(h2_esp_net_join_multicast(7, &invalid) == H2_PAL_ERR_INVALID_ARG);
  assert(calls == 2u);
  option_result = 0;
  assert(h2_esp_net_join_multicast(7, &expected) == H2_PAL_OK);
  option_result = -1;
  assert(h2_esp_net_join_multicast(7, &expected) == H2_PAL_ERR_IO);
  assert(calls == 4u);
  return 0;
}
