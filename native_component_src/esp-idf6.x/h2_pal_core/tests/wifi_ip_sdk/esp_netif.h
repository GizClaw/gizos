#ifndef H2_TEST_ESP_NETIF_H
#define H2_TEST_ESP_NETIF_H
#include "lwip/netif.h"
#define ESP_OK 0
typedef struct {
  uint32_t addr;
} esp_ip4_addr_t;
typedef struct {
  uint32_t addr[4];
} esp_ip6_addr_t;
typedef struct {
  esp_ip4_addr_t ip, netmask, gw;
} esp_netif_ip_info_t;
typedef struct {
  struct netif n;
  esp_netif_ip_info_t info;
} esp_netif_t;
void *esp_netif_get_netif_impl(esp_netif_t *);
int esp_netif_is_netif_up(esp_netif_t *);
int esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
int esp_netif_get_all_preferred_ip6(esp_netif_t *, esp_ip6_addr_t *);
#endif
