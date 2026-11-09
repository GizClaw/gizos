#ifndef STREAM_TEST_UART_H
#define STREAM_TEST_UART_H
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>
typedef int uart_port_t;
typedef int uart_word_length_t;
typedef int uart_hw_flowcontrol_t;
#define UART_PARITY_DISABLE 0
#define UART_PARITY_EVEN 1
#define UART_PARITY_ODD 2
#define UART_STOP_BITS_1 1
#define UART_STOP_BITS_2 2
#define UART_SCLK_DEFAULT 0
typedef struct {
  int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;
esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int queue,
                              void* out_queue, int flags);
esp_err_t uart_param_config(uart_port_t port, const uart_config_t* config);
esp_err_t uart_flush_input(uart_port_t port);
int uart_read_bytes(uart_port_t port, void* data, uint32_t len,
                    TickType_t ticks);
int uart_write_bytes(uart_port_t port, const void* data, size_t len);
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks);
#endif
