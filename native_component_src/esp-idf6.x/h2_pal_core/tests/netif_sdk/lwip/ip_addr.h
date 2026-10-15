#ifndef TEST_NETIF_IP_ADDR_H
#define TEST_NETIF_IP_ADDR_H

#include <stdint.h>
#include <string.h>

typedef uint8_t u8_t;
typedef struct {
  uint32_t addr;
} ip4_addr_t;
typedef struct {
  uint32_t addr[4];
  uint8_t zone;
} ip6_addr_t;
typedef struct {
  union {
    ip4_addr_t ip4;
    ip6_addr_t ip6;
  } u_addr;
  uint8_t type;
} test_ip_addr_t;
#if LWIP_IPV4 && LWIP_IPV6
typedef test_ip_addr_t ip_addr_t;
#define IP_IS_V4(ip) ((ip)->type == 0u)
#define IP_IS_V6(ip) ((ip)->type == 6u)
#define ip_2_ip4(ip) (&(ip)->u_addr.ip4)
#define ip_2_ip6(ip) (&(ip)->u_addr.ip6)
#define ip6_addr_isany(ip) test_ip6_isany(ip)
#define ip_addr_cmp(a, b) test_ip_equal((a), (b))
#define ip_addr_isany(ip) test_ip_isany(ip)
static inline int test_ip6_isany(const ip6_addr_t *ip) {
  static const uint32_t zero[4] = {0};
  return memcmp(ip->addr, zero, sizeof(zero)) == 0;
}
static inline int test_ip_equal(const ip_addr_t *a, const ip_addr_t *b) {
  return a->type == b->type &&
         (a->type == 0u ? a->u_addr.ip4.addr == b->u_addr.ip4.addr
                        : memcmp(a->u_addr.ip6.addr, b->u_addr.ip6.addr,
                                 sizeof(a->u_addr.ip6.addr)) == 0 &&
                              a->u_addr.ip6.zone == b->u_addr.ip6.zone);
}
static inline int test_ip_isany(const ip_addr_t *ip) {
  static const uint32_t zero[4] = {0};
  return ip->type == 0u ? ip->u_addr.ip4.addr == 0u
                        : memcmp(ip->u_addr.ip6.addr, zero, sizeof(zero)) == 0;
}
#else
typedef ip4_addr_t ip_addr_t;
#define IP_IS_V4(ip) ((void)(ip), 1)
#define ip_2_ip4(ip) ((const ip4_addr_t *)(ip))
#define ip_addr_cmp(a, b) ((a)->addr == (b)->addr)
#define ip_addr_isany(ip) ((ip)->addr == 0u)
#endif
#define ip_addr_copy(dst, src) ((dst) = (src))
#define IPADDR_STRLEN_MAX 48
char *ipaddr_ntoa_r(const ip_addr_t *ip, char *buffer, int length);

#endif
