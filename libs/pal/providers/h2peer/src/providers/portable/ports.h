#ifndef PORTS_H_
#define PORTS_H_

#include "address.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_time.h"

int ports_resolve_addr(const h2_pal_net_api_t* net, const char* host,
                       h2_pal_net_addr_t* addr);
int ports_get_host_addr(const h2_pal_net_api_t* net, h2_pal_net_addr_t* addr,
                        const char* iface_prefix);
void ports_sleep_ms(const h2_pal_time_api_t* time, int ms);

#endif  // PORTS_H_
