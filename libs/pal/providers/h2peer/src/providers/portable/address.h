#ifndef ADDRESS_H_
#define ADDRESS_H_

#include <stddef.h>
#include <stdint.h>

#include "h2/pal/net/h2_pal_net.h"

#define H2_PEER_NET_ADDR_STRING_SIZE 40u

int h2_peer_net_addr_family_is_valid(h2_pal_net_family_t family);

int h2_peer_net_addr_format(
    const h2_pal_net_addr_t* addr, char* buf, size_t len);

#endif  // ADDRESS_H_
