#ifndef H2_BK_UART_IO_TEST_SDK_H
#define H2_BK_UART_IO_TEST_SDK_H
#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_SYS_PRINT_DEV_UART 1
#define CONFIG_UART_PRINT_PORT 1
#define CONFIG_UART_PRINT_BAUD_RATE 460800
#define BK_OK 0
#define kNoErr 0
#define SHELL_IO_CTRL_TX_SUSPEND 1
#define SHELL_IO_CTRL_TX_RESUME 2
typedef int uart_id_t;
typedef int beken_mutex_t;
typedef struct shell_dev shell_dev_t;
typedef struct { int (*io_ctrl)(shell_dev_t *, int, void *); } shell_drv_t;
struct shell_dev { shell_drv_t *dev_drv; };
extern shell_dev_t shell_uart;
int rtos_init_mutex(beken_mutex_t *);
int rtos_deinit_mutex(beken_mutex_t *);
int rtos_trylock_mutex(beken_mutex_t *);
int rtos_unlock_mutex(beken_mutex_t *);
uint32_t rtos_get_time(void);
uint32_t rtos_enter_critical(void);
void rtos_exit_critical(uint32_t);
void rtos_delay_milliseconds(uint32_t);
int bk_uart_set_baud_rate(uart_id_t, uint32_t);
int bk_uart_take_rx_isr(uart_id_t, void (*)(uart_id_t, void *), void *);
int bk_uart_enable_rx_interrupt(uart_id_t);
int bk_uart_disable_rx_interrupt(uart_id_t);
int bk_uart_recover_rx_isr(uart_id_t);
int bk_uart_set_enable_tx(uart_id_t, bool);
bool bk_uart_is_tx_over(uart_id_t);
int uart_read_byte_ex(uart_id_t, uint8_t *);
int uart_write_ready(uart_id_t);
int uart_write_byte(uart_id_t, uint8_t);
#endif
