#ifndef H2_BK_WIFI_IPV6_SYNC_H
#define H2_BK_WIFI_IPV6_SYNC_H
#include "h2_bk_wifi_ipv6_internal.h"
#include "lwip/netif.h"
/* TCP/IP context only. CP owns SLAAC/DAD; AP installs its preferred snapshot.
 */
void h2_bk_wifi_ipv6_clear(struct netif *sta);
err_t h2_bk_wifi_ipv6_install(struct netif *sta,
                              const h2_bk_wifi_ipv6_snapshot_t *snapshot,
                              h2_pal_wifi_sta_status_t *status,
                              uint32_t *cp_generation);
#endif
