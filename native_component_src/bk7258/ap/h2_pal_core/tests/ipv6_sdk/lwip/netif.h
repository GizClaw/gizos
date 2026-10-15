#ifndef H2_TEST_NETIF_H
#define H2_TEST_NETIF_H
#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#define LWIP_IPV6 1
#define LWIP_IPV6_NUM_ADDRESSES 3
#define IP6_ADDR_INVALID 0
#define IP6_ADDR_PREFERRED 0x30
#define ERR_OK 0
#define ERR_IF -1
#define ERR_BUF -2
#define IP6_UNICAST 0
#define lwip_ntohl ntohl
typedef int err_t;
typedef struct {
  uint32_t addr[4];
  uint8_t zone;
} ip6_addr_t;
typedef struct {
  uint32_t addr;
} ip4_addr_t;
extern const ip6_addr_t test_ip6_zero;
#define IP6_ADDR_ANY6 (&test_ip6_zero)
struct netif {
  ip6_addr_t ip6[3];
  uint8_t state[3];
  ip4_addr_t ip4, mask, gw;
  unsigned up, link, autoconfig, index;
};
extern struct netif *netif_default;
#define netif_ip6_addr(n, i) (test_capture_edge(), &(n)->ip6[(i)])
void test_capture_edge(void);
#define netif_ip6_addr_state(n, i) ((n)->state[(i)])
#define netif_is_up(n) ((n)->up)
#define ip6_addr_ispreferred(s) ((s) == IP6_ADDR_PREFERRED)
#define netif_set_ip6_autoconfig_enabled(n, s) ((n)->autoconfig = (s))
#define netif_ip4_addr(n) (&(n)->ip4)
#define netif_ip4_netmask(n) (&(n)->mask)
#define netif_ip4_gw(n) (&(n)->gw)
#define ip4_addr_get_u32(a) ((a)->addr)
#define ip4_addr_set_zero(a) ((a)->addr = 0)
static inline int ip6_addr_islinklocal(const ip6_addr_t *a) {
  const uint8_t *p = (const uint8_t *)a->addr;
  return p[0] == 0xfe && (p[1] & 0xc0) == 0x80;
}
static inline void ip6_addr_assign_zone(ip6_addr_t *a, int type,
                                        struct netif *n) {
  (void)type;
  a->zone = ip6_addr_islinklocal(a) ? n->index : 0;
}
void netif_ip6_addr_set_state(struct netif *, int, unsigned);
void netif_ip6_addr_set(struct netif *, int, const ip6_addr_t *);
void netif_set_link_down(struct netif *);
void netif_set_link_up(struct netif *);
void netif_set_down(struct netif *);
void netif_set_up(struct netif *);
void netif_set_addr(struct netif *, const ip4_addr_t *, const ip4_addr_t *,
                    const ip4_addr_t *);
void netif_set_default(struct netif *);
void netif_create_ip6_linklocal_address(struct netif *, int);
#endif
