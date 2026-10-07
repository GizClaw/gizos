#ifndef H2_IPV6_RUNNER_H
#define H2_IPV6_RUNNER_H
#include "h2_pal_ipv6_e2e.h"
int h2_ipv6_parse_address(const h2_pal_net_api_t *, const char *,
                          h2_pal_net_addr_t *);
void h2_ipv6_report(void *, const h2_net_tls_case_result_t *);
int h2_ipv6_write_report(const char *, const char *,
                         const h2_pal_ipv6_result_t *, int, int);
#endif
