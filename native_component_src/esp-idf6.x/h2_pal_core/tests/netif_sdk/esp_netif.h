#ifndef TEST_ESP_NETIF_H
#define TEST_ESP_NETIF_H
#include "lwip/ip_addr.h"
#include <stdbool.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_IPADDR_TYPE_V4 0
#define ESP_IPADDR_TYPE_V6 6
typedef enum {
  ESP_NETIF_DNS_MAIN,
  ESP_NETIF_DNS_BACKUP,
  ESP_NETIF_DNS_FALLBACK,
  ESP_NETIF_DNS_MAX
} esp_netif_dns_type_t;
typedef struct {
  test_ip_addr_t ip;
} esp_netif_dns_info_t;
typedef struct {
  ip4_addr_t ip, netmask, gw;
} esp_netif_ip_info_t;
#if LWIP_IPV6
#define CONFIG_LWIP_IPV6_NUM_ADDRESSES 3
typedef ip6_addr_t esp_ip6_addr_t;
#endif
typedef struct esp_netif {
  const char *key;
  int index;
  esp_netif_dns_info_t dns[ESP_NETIF_DNS_MAX];
  esp_err_t dns_result[ESP_NETIF_DNS_MAX];
#if LWIP_IPV6
  esp_ip6_addr_t ipv6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
  int ipv6_count;
#endif
} esp_netif_t;
esp_err_t esp_netif_init(void);
esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *context);
esp_netif_t *esp_netif_get_default_netif(void);
esp_netif_t *esp_netif_next_unsafe(esp_netif_t *netif);
const char *esp_netif_get_ifkey(esp_netif_t *netif);
int esp_netif_get_netif_impl_index(esp_netif_t *netif);
bool esp_netif_is_netif_up(esp_netif_t *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip);
#if LWIP_IPV6
int esp_netif_get_all_ip6(esp_netif_t *netif, esp_ip6_addr_t *addresses);
#endif
esp_err_t esp_netif_get_mac(esp_netif_t *netif, uint8_t *mac);
esp_err_t esp_netif_get_mtu(esp_netif_t *netif, uint16_t *mtu);
esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type,
                                 esp_netif_dns_info_t *dns);
esp_err_t esp_netif_set_default_netif(esp_netif_t *netif);
size_t test_netif_strlcpy(char *dst, const char *src, size_t size);
#define strlcpy test_netif_strlcpy
#endif
