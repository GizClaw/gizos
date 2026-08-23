#include "app_config.h"

#include "asm/uart_dev.h"
#include "system/includes.h"

const struct task_info task_info_table[] = {
    {"app_core", 1, 896, 768},
    {"sys_event", 6, 256, 0},
    {"systimer", 6, 128, 0},
    {0, 0},
};

#if TCFG_UART0_ENABLE
// clang-format off
UART0_PLATFORM_DATA_BEGIN(uart0_data)
    .tx_pin = TCFG_UART0_TX_PORT,
    .rx_pin = TCFG_UART0_RX_PORT,
    .baudrate = TCFG_UART0_BAUDRATE,
    .flags = UART_DEBUG,
UART0_PLATFORM_DATA_END()
#endif

void debug_uart_init(const struct uart_platform_data *data) {
#if TCFG_UART0_ENABLE
  uart_init(data != NULL ? data : &uart0_data);
#else
  (void)data;
#endif
}

void board_init(void) {}
// clang-format on
