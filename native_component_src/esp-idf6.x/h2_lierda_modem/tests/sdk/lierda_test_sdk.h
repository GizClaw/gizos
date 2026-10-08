#ifndef LIERDA_TEST_SDK_H
#define LIERDA_TEST_SDK_H

#include "h2/pal/net/h2_pal_netif.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_NOT_SUPPORTED 5
#define UART_NUM_MAX 3
#define GPIO_NUM_MAX 49
#define UART_DATA_8_BITS 8
#define UART_STOP_BITS_1 1
#define UART_PARITY_DISABLE 0
#define ESP_MODEM_FLOW_CONTROL_NONE 0
#define ESP_MODEM_DEFAULT_UART_CLK 0
#define configMAX_PRIORITIES 25u
#ifndef CONFIG_LWIP_PPP_SUPPORT
#define CONFIG_LWIP_PPP_SUPPORT 1
#endif
#ifndef CONFIG_LWIP_IPV4
#define CONFIG_LWIP_IPV4 1
#endif
#ifndef CONFIG_LWIP_PPP_NOTIFY_PHASE_SUPPORT
#define CONFIG_LWIP_PPP_NOTIFY_PHASE_SUPPORT 1
#endif
#ifndef CONFIG_LWIP_PPP_PAP_SUPPORT
#define CONFIG_LWIP_PPP_PAP_SUPPORT 1
#endif
#ifndef CONFIG_LWIP_PPP_CHAP_SUPPORT
#define CONFIG_LWIP_PPP_CHAP_SUPPORT 1
#endif
#define BIT0 1u
#define BIT1 2u
#define BIT2 4u
#define pdFALSE 0
#define pdMS_TO_TICKS(value) (value)
typedef uint32_t EventBits_t;
typedef struct sdk_events *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t group);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
    int clear, int all, uint32_t timeout);

typedef int esp_event_base_t;
#define IP_EVENT 1
#define NETIF_PPP_STATUS 2
#define ESP_EVENT_ANY_ID -1
#define IP_EVENT_PPP_GOT_IP 10
#define IP_EVENT_PPP_LOST_IP 11
#define NETIF_PPP_PHASE_DEAD 20
#define NETIF_PPP_PHASE_DISCONNECT 21
#define NETIF_PPP_ERRORCONNECT 22
#define NETIF_PPP_ERRORAUTHFAIL 23
#define NETIF_PPP_ERRORPEERDEAD 24
#define NETIF_PPP_CONNECT_FAILED 25
typedef struct sdk_handler *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    esp_event_handler_t callback, void *user, esp_event_handler_instance_t *out);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance);

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;
typedef struct esp_netif { esp_netif_ip_info_t ip; } esp_netif_t;
typedef struct { int unused; } esp_netif_config_t;
#define ESP_NETIF_DEFAULT_PPP() ((esp_netif_config_t){0})
typedef struct { esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct {
    struct { int type; union { esp_ip4_addr_t ip4; } u_addr; } ip;
} esp_netif_dns_info_t;
#define ESP_IPADDR_TYPE_V4 0
#define ESP_NETIF_DNS_MAIN 0
#define ESP_NETIF_DNS_BACKUP 1
typedef struct { bool ppp_phase_event_enabled; bool ppp_error_event_enabled; } esp_netif_ppp_config_t;
typedef int esp_netif_auth_type_t;
#define NETIF_PPP_AUTHTYPE_NONE 0
#define NETIF_PPP_AUTHTYPE_PAP 1
#define NETIF_PPP_AUTHTYPE_CHAP 2
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
void esp_netif_destroy(esp_netif_t *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out);
esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip);
esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *out);
esp_err_t esp_netif_ppp_set_params(esp_netif_t *netif, const esp_netif_ppp_config_t *config);
esp_err_t esp_netif_ppp_set_auth(esp_netif_t *netif, esp_netif_auth_type_t type,
    const char *username, const char *password);
void h2_esp_platform_netif_register(esp_netif_t *netif, h2_pal_netif_kind_t kind);
void h2_esp_platform_netif_unregister(esp_netif_t *netif);
h2_pal_result_t h2_esp_platform_netif_reconcile_default(void);
h2_pal_result_t h2_esp_platform_ppp_quiesce(void *netif_handle, uint32_t timeout_ms);

typedef struct {
    size_t dte_buffer_size;
    uint32_t task_stack_size;
    uint32_t task_priority;
    struct {
        int port_num, data_bits, stop_bits, parity, flow_control, source_clk;
        int baud_rate, tx_io_num, rx_io_num, rts_io_num, cts_io_num;
        size_t rx_buffer_size, tx_buffer_size, event_queue_size;
    } uart_config;
} esp_modem_dte_config_t;
typedef struct { const char *apn; } esp_modem_dce_config_t;
#define ESP_MODEM_DCE_DEFAULT_CONFIG(value) ((esp_modem_dce_config_t){.apn = (value)})
typedef struct esp_modem_dce { esp_netif_t *netif; char apn[64]; } esp_modem_dce_t;
#define ESP_MODEM_MODE_COMMAND 0
#define ESP_MODEM_MODE_DATA 1
esp_modem_dce_t *esp_modem_new(const esp_modem_dte_config_t *dte,
    const esp_modem_dce_config_t *dce, esp_netif_t *netif);
void esp_modem_destroy(esp_modem_dce_t *dce);
esp_err_t esp_modem_at(esp_modem_dce_t *dce, const char *cmd, char *response, int timeout);
esp_err_t esp_modem_set_apn(esp_modem_dce_t *dce, const char *apn);
esp_err_t esp_modem_set_mode(esp_modem_dce_t *dce, int mode);

#endif
