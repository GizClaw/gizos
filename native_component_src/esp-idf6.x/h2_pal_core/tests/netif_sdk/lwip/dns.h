#ifndef TEST_NETIF_DNS_H
#define TEST_NETIF_DNS_H
#include "lwip/ip_addr.h"
#define DNS_MAX_SERVERS 3
#define DNS_FALLBACK_SERVER_INDEX 2
const ip_addr_t *dns_getserver(u8_t index);
void dns_setserver(u8_t index, const ip_addr_t *server);
void dns_clear_cache(void);
#endif
