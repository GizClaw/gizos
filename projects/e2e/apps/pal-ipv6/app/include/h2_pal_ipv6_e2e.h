#ifndef H2_PAL_IPV6_E2E_H
#define H2_PAL_IPV6_E2E_H
#include "h2/pal/net/h2_pal_dtls.h"
#include "h2_pal_net_tls_e2e.h"
#ifdef __cplusplus
extern "C" {
#endif
#define H2_PAL_IPV6_EXTRA_CASES 19u
#define H2_PAL_IPV6_CASES (37u + H2_PAL_IPV6_EXTRA_CASES)
typedef struct h2_pal_ipv6_config {
  h2_net_tls_config_t transport;
  const h2_pal_dtls_api_t *dtls;
  const char *dual_dns_host;
  const char *offer_url;
  const char *stun_url;
  const char *http_url;
  const char *fallback_url;
  const char *mqtt_host;
  uint16_t mqtt_port;
  /* Concrete interface from the actual Netif provider, never inferred globally.
   */
  h2_pal_netif_ref_t netif;
  h2_pal_net_addr_t local_ipv6;
  h2_pal_net_addr_t dns_server;
  h2_pal_net_addr_t dns_answer;
} h2_pal_ipv6_config_t;
typedef struct h2_pal_ipv6_result {
  unsigned passed, failed, blocked;
  size_t retained_sockets, retained_resolvers, retained_allocations;
  h2_net_tls_case_result_t cases[H2_PAL_IPV6_CASES];
} h2_pal_ipv6_result_t;
/* Borrow providers and fixture through return; execute all mandatory transport
 * cases on IPv6 and the address-selection/application cases below. */
int h2_pal_ipv6_e2e_run(const h2_pal_ipv6_config_t *, h2_pal_ipv6_result_t *);
#ifdef __cplusplus
}
#endif
#endif
