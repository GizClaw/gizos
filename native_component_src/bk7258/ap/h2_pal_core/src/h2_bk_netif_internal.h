#ifndef H2_BK_NETIF_INTERNAL_H
#define H2_BK_NETIF_INTERNAL_H
#include "h2/pal/net/h2_pal_netif.h"
/* Resolve a named prefix against one consistent lwIP snapshot. */
h2_pal_result_t h2_bk_netif_address_for_prefix(
    const char *prefix, h2_pal_net_addr_t *out_addr);
#endif
