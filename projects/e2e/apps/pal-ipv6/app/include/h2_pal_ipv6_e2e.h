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
typedef struct h2_pal_ipv6_cleanup h2_pal_ipv6_cleanup_t;
typedef struct h2_pal_ipv6_result {
  unsigned passed, failed, blocked;
  size_t retained_sockets, retained_resolvers, retained_allocations;
  size_t retained_tasks;
  int cleanup_error;
  h2_pal_ipv6_cleanup_t *retained_cleanup;
  /* Owned by the mobile adapter after failed cleanup, never a stack Runtime. */
  h2_runtime_t *retained_runtime;
  h2_net_tls_case_result_t cases[H2_PAL_IPV6_CASES];
} h2_pal_ipv6_result_t;
/* The result must initially be zeroed. Providers remain borrowed until cleanup
 * succeeds, including when a failed join retains a local worker after return.
 * A result with retained_cleanup cannot be reused for another run. */
int h2_pal_ipv6_e2e_run(const h2_pal_ipv6_config_t *, h2_pal_ipv6_result_t *);
/* Retry retained worker cleanup. Failure preserves every handle and borrow;
 * success releases the worker, listener and its owned context exactly once. */
int h2_pal_ipv6_e2e_cleanup(h2_pal_ipv6_result_t *);
#ifdef __cplusplus
}
#endif
#endif
