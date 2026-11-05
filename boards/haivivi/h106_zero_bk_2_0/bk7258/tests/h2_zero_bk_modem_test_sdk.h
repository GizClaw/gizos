#ifndef H2_ZERO_BK_MODEM_TEST_SDK_H
#define H2_ZERO_BK_MODEM_TEST_SDK_H
#include "h2_quectel_modem.h"
#include "h2/pal/net/h2_pal_netif.h"
#include <stdbool.h>
#include <stdint.h>

typedef int uart_id_t;
typedef int gpio_id_t;
typedef int bk_err_t;
typedef void* beken_thread_t;
typedef void* beken_semaphore_t;
typedef uint32_t u32_t;
typedef uint8_t u8_t;
typedef int8_t err_t;
typedef struct {
  int io_mode, pull_mode, func_mode;
} gpio_config_t;
typedef struct {
  int baud_rate, data_bits, parity, stop_bits, flow_ctrl, src_clk;
} uart_config_t;
typedef struct {
  uint32_t addr;
} ip_addr_t;
struct netif {
  ip_addr_t ip;
};
typedef struct {
  int unused;
} ppp_pcb;
#define BK_OK 0
#define kNoErr 0
#define ERR_OK 0
#define PPPERR_NONE 0
#define PPPAUTHTYPE_PAP 1
#define PPPAUTHTYPE_CHAP 2
#define BK_ERR_UART_RX_TIMEOUT (-2)
#define GPIO_OUTPUT_ENABLE 1
#define GPIO_PULL_DISABLE 0
#define GPIO_SECOND_FUNC_DISABLE 0
#define GPIO_DEV_UART2_TXD 1
#define GPIO_DEV_UART2_RXD 2
#define UART_DATA_8_BITS 8
#define UART_PARITY_NONE 0
#define UART_STOP_BITS_1 1
#define UART_FLOWCTRL_DISABLE 0
#define UART_SCLK_XTAL_26M 0
#define UART_RX_STOP_DETECT_TIME_256_BITS 0
#define BEKEN_DEFAULT_WORKER_PRIORITY 6
#define H2_ZERO_BK_MODEM_UART_ID 2
#define H2_ZERO_BK_MODEM_UART_BAUD 115200
#define H2_ZERO_BK_MODEM_UART_TX_GPIO 10
#define H2_ZERO_BK_MODEM_UART_RX_GPIO 11
#define H2_ZERO_BK_MODEM_ENABLE_GPIO 6
#define H2_ZERO_BK_MODEM_CELL_LOCATE_TOKEN ""
#define netif_ip4_addr(n) (&(n)->ip)
#define ip4_addr_get_u32(a) ((a)->addr)
#define ip_2_ip4(a) (a)

int gpio_dev_unmap(gpio_id_t);
int gpio_dev_map(gpio_id_t, int);
int bk_gpio_set_config(gpio_id_t, const gpio_config_t*);
int bk_gpio_set_output_high(gpio_id_t);
int bk_gpio_set_output_low(gpio_id_t);
int bk_gpio_pull_up(gpio_id_t);
uint32_t bk_gpio_get_value(gpio_id_t);
int bk_uart_init(uart_id_t, const uart_config_t*);
int bk_uart_deinit(uart_id_t);
int bk_uart_set_enable_rx(uart_id_t, bool);
int bk_uart_set_enable_tx(uart_id_t, bool);
int bk_uart_set_rx_timeout(uart_id_t, uint32_t);
int bk_uart_enable_rx_interrupt(uart_id_t);
int bk_uart_read_bytes(uart_id_t, void*, uint32_t, uint32_t);
int bk_uart_write_bytes(uart_id_t, const void*, uint32_t);
void bk_uart_wait_tx_over(uart_id_t);
void rtos_delay_milliseconds(uint32_t);
uint32_t rtos_get_time(void);
int rtos_init_semaphore(beken_semaphore_t*, int);
int rtos_deinit_semaphore(beken_semaphore_t*);
int rtos_set_semaphore(beken_semaphore_t*);
int rtos_get_semaphore(beken_semaphore_t*, uint32_t);
int rtos_thread_join(beken_thread_t*);
int rtos_delete_thread(beken_thread_t*);
int rtos_core0_create_psram_thread(beken_thread_t*, uint8_t, const char*,
                                   void (*)(void*), uint32_t, void*);
ppp_pcb* pppapi_pppos_create(struct netif*,
                             u32_t (*)(ppp_pcb*, u8_t*, u32_t, void*),
                             void (*)(ppp_pcb*, int, void*), void*);
err_t pppapi_close(ppp_pcb*, u8_t);
err_t pppapi_free(ppp_pcb*);
err_t pppapi_connect(ppp_pcb*, uint16_t);
err_t pppos_input_tcpip(ppp_pcb*, u8_t*, int);
void ppp_set_usepeerdns(ppp_pcb*, u8_t);
void ppp_set_auth(ppp_pcb*, u8_t, const char*, const char*);
const ip_addr_t* dns_getserver(u8_t);
void h2_bk_platform_netif_register(struct netif*, h2_pal_netif_kind_t);
void h2_bk_platform_netif_unregister(struct netif*);
h2_pal_result_t h2_bk_platform_netif_reconcile_default_async(void);
h2_pal_result_t h2_zero_bk_peripheral_power_enable(void);
const h2_pal_sync_api_t* h2_bk7258_board_sync_api(void);
h2_pal_mem_api_t* h2_bk7258_board_default_allocator(void);
const h2_pal_system_event_api_t* h2_bk7258_board_system_event_api(void);
const char* h2_apn_from_imsi(const char*);
#endif
