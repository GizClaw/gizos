#ifndef H2_TEST_PPP_ESP_NETIF_H
#define H2_TEST_PPP_ESP_NETIF_H
#include "lwip/netif.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_NETIF_FLAG_IS_PPP (1u << 5)
typedef unsigned esp_netif_flags_t;
typedef struct esp_netif {
  esp_netif_flags_t flags;
  struct netif *impl;
} esp_netif_t;
esp_netif_flags_t esp_netif_get_flags(esp_netif_t *netif);
esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *user);
#endif
