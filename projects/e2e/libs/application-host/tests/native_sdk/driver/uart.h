#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef int uart_port_t;
#define CONFIG_ESP_CONSOLE_UART 1
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 0
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200
static inline esp_err_t uart_driver_install(uart_port_t port, int rx, int tx,
                                            int n, void *queue, int flags) {
  (void)port;
  (void)rx;
  (void)tx;
  (void)n;
  (void)queue;
  (void)flags;
  return ESP_OK;
}
static inline esp_err_t uart_set_baudrate(uart_port_t port, uint32_t baud) {
  (void)port;
  (void)baud;
  return ESP_OK;
}
static inline esp_err_t uart_flush_input(uart_port_t port) {
  (void)port;
  return ESP_OK;
}
