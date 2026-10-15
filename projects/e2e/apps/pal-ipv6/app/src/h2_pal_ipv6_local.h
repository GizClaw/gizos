#ifndef H2_PAL_IPV6_LOCAL_H
#define H2_PAL_IPV6_LOCAL_H

#include "h2_pal_ipv6_e2e.h"

typedef int (*h2_pal_ipv6_http_call_t)(void *, const char *);
int h2_pal_ipv6_local_http(const h2_runtime_t *, const char *,
                           h2_pal_ipv6_http_call_t, void *,
                           h2_pal_ipv6_cleanup_t **);
int h2_pal_ipv6_local_cleanup(h2_pal_ipv6_cleanup_t **);
int h2_pal_ipv6_local_cleanup_error(const h2_pal_ipv6_cleanup_t *);
int h2_pal_ipv6_local_isolation(const h2_pal_net_api_t *);

#endif
