#ifndef H2_IPERF_AP_TEST_SDK_H
#define H2_IPERF_AP_TEST_SDK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 1
#define ERR_OK 0
#define IPADDR_TYPE_V6 6
#define IP6_NEXTH_ICMP6 58
#define IP6_MULTICAST 1
#define PBUF_IP 0
#define PBUF_RAM 0
typedef struct {
  uint32_t addr[4];
} ip6_addr_t;
typedef ip6_addr_t esp_ip6_addr_t;
typedef struct {
  uint32_t addr;
} esp_ip4_addr_t;
typedef struct {
  esp_ip4_addr_t ip;
} esp_netif_ip_info_t;
typedef struct {
  ip6_addr_t ip6;
  int type;
} ip_addr_t;
struct netif {
  uint8_t hwaddr[6];
  ip6_addr_t link_local;
};
struct raw_pcb {
  unsigned ttl, chksum_reqd, chksum_offset;
};
struct pbuf {
  unsigned value;
};
typedef struct {
  unsigned value;
} esp_netif_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip)                                                             \
  ((unsigned)(ip)->addr >> 24), (((unsigned)(ip)->addr >> 16) & 255),          \
      (((unsigned)(ip)->addr >> 8) & 255), ((unsigned)(ip)->addr & 255)
#define IP_SET_TYPE_VAL(ip, family) ((ip).type = (family))
#define ip_2_ip6(ip) (&(ip)->ip6)
#define ip6_addr_copy(dst, src) ((dst) = (src))
#define netif_ip6_addr(netif, index) ((void)(index), &(netif)->link_local)
int ip6addr_aton(const char *text, ip6_addr_t *out);
void ip6_addr_assign_zone(ip6_addr_t *ip, int type, struct netif *netif);
struct pbuf *pbuf_alloc(int layer, size_t size, int type);
int pbuf_take(struct pbuf *buffer, const void *data, size_t size);
void pbuf_free(struct pbuf *buffer);
struct raw_pcb *raw_new_ip_type(int type, int protocol);
void raw_bind_netif(struct raw_pcb *pcb, struct netif *netif);
void raw_set_multicast_ttl(struct raw_pcb *pcb, unsigned ttl);
int raw_sendto_if_src(struct raw_pcb *pcb, struct pbuf *buffer,
                      const ip_addr_t *destination, struct netif *netif,
                      const ip_addr_t *source);
void raw_remove(struct raw_pcb *pcb);
void sys_timeout(uint32_t timeout, void (*callback)(void *), void *user);
void sys_untimeout(void (*callback)(void *), void *user);
int tcpip_callback_wait(void (*callback)(void *), void *user);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif);
esp_err_t esp_netif_set_ip_info(esp_netif_t *netif,
                                const esp_netif_ip_info_t *info);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out);
esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *netif);
esp_err_t esp_netif_add_ip6_address(esp_netif_t *netif, esp_ip6_addr_t address,
                                    bool preferred);
esp_err_t esp_netif_get_ip6_linklocal(esp_netif_t *netif, esp_ip6_addr_t *out);
struct netif *esp_netif_get_netif_impl(esp_netif_t *netif);
#endif
