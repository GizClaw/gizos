#ifndef H2_BK_NET_ADDR_H
#define H2_BK_NET_ADDR_H
#include "h2/pal/net/h2_pal_net.h"
#include <lwip/sockets.h>
int h2_bk_net_family(h2_pal_net_family_t family);
int h2_bk_net_to_sockaddr(const h2_pal_net_addr_t *addr,
                          struct sockaddr_storage *out, socklen_t *out_size);
int h2_bk_net_from_sockaddr(const struct sockaddr *addr,
                            h2_pal_net_addr_t *out);
h2_pal_result_t h2_bk_net_resolve_all(void *user, const char *host,
                                      h2_pal_net_family_t family,
                                      h2_pal_net_addr_list_t *out);
h2_pal_net_addr_t h2_bk_net_first_addr(const h2_pal_net_addr_list_t *addresses);
#endif
