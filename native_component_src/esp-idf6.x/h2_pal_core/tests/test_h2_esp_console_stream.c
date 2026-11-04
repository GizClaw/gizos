#include "h2_esp_platform_core.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"

#include <assert.h>
#include <string.h>

static unsigned s_uart_installs, s_uart_routes, s_uart_writes, s_uart_drains;
static unsigned s_usb_installs, s_usb_routes, s_usb_uninstalls;
static int s_usb_installed = 1, s_install_error = ESP_ERR_TIMEOUT;
static int s_uart_error;

esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int queue,
                              void* out_queue, int flags) {
  assert(port == 0 && rx == 8192 && tx == 2048);
  assert(!queue && !out_queue && !flags);
  ++s_uart_installs;
  return ESP_ERR_INVALID_STATE;
}
esp_err_t uart_param_config(uart_port_t port, const uart_config_t* config) {
  assert(port == 0 && config->baud_rate == 460800);
  assert(config->data_bits == 3 && config->stop_bits == UART_STOP_BITS_1);
  assert(config->parity == UART_PARITY_DISABLE);
  return ESP_OK;
}
esp_err_t uart_flush_input(uart_port_t port) {
  assert(port == 0);
  return ESP_OK;
}
void uart_vfs_dev_use_driver(uart_port_t port) {
  assert(port == 0);
  ++s_uart_routes;
}
int uart_read_bytes(uart_port_t port, void* data, uint32_t len,
                    TickType_t ticks) {
  assert(port == 0 && len && ticks >= 1u);
  *(char*)data = 'u';
  return 1;
}
int uart_write_bytes(uart_port_t port, const void* data, size_t len) {
  assert(port == 0 && data && s_uart_routes && s_uart_drains);
  ++s_uart_writes;
  return (int)len;
}
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks) {
  assert(port == 0 && ticks >= 1u);
  ++s_uart_drains;
  return s_uart_error;
}
bool usb_serial_jtag_is_driver_installed(void) { return s_usb_installed; }
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks) {
  assert(ticks);
  return ESP_OK;
}
esp_err_t usb_serial_jtag_driver_uninstall(void) {
  s_usb_installed = 0;
  ++s_usb_uninstalls;
  return ESP_OK;
}
esp_err_t usb_serial_jtag_driver_install(
    usb_serial_jtag_driver_config_t* config) {
  assert(config->rx_buffer_size == 8192u && config->tx_buffer_size == 2048u);
  ++s_usb_installs;
  s_usb_installed = s_install_error == ESP_OK;
  return s_install_error;
}
void usb_serial_jtag_vfs_use_driver(void) { ++s_usb_routes; }
int usb_serial_jtag_read_bytes(void* data, size_t len, TickType_t ticks) {
  assert(len && ticks >= 1u);
  *(char*)data = 'j';
  return 1;
}
int usb_serial_jtag_write_bytes(const void* data, size_t len,
                                TickType_t ticks) {
  assert(data && ticks >= 1u && s_usb_routes);
  return (int)len;
}

int main(void) {
  const h2_pal_uart_io_stream_api_t* uart =
      h2_esp_platform_uart_io_stream_api();
  h2_pal_uart_io_stream_config_t uc = {
      .baud_rate = 460800u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .rx_buffer_size = 8192u,
      .tx_buffer_size = 2048u,
      .flags = H2_PAL_UART_IO_STREAM_CONFIG_ROUTE_STDIO |
               H2_PAL_UART_IO_STREAM_CONFIG_FLUSH_INPUT,
  };
  assert(h2_pal_uart_io_stream_configure(uart, &uc) == H2_PAL_OK);
  assert(s_uart_installs == 1u && s_uart_routes == 1u);
  uint8_t bytes[1024] = {0};
  size_t count;
  assert(h2_pal_uart_io_stream_write(uart, bytes, sizeof(bytes), &count, 1u) ==
         H2_PAL_OK);
  assert(count == sizeof(bytes) && s_uart_writes == 1u && s_uart_drains == 1u);
  s_uart_error = ESP_ERR_TIMEOUT;
  assert(h2_pal_uart_io_stream_write(uart, bytes, sizeof(bytes), &count, 1u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(!count && s_uart_writes == 1u);
  s_uart_error = 0;
  assert(h2_pal_uart_io_stream_read(uart, bytes, sizeof(bytes), &count, 1u) ==
         H2_PAL_OK);
  assert(count == 1u && bytes[0] == 'u');
  assert(h2_pal_uart_io_stream_release(uart) == H2_PAL_OK);
  assert(h2_pal_uart_io_stream_release(uart) == H2_PAL_OK);
  assert(s_uart_installs == 1u && s_uart_routes == 1u);
  assert(h2_pal_uart_io_stream_read(uart, bytes, sizeof(bytes), &count, 1u) ==
         H2_PAL_OK);
  uc.data_bits = 4u;
  assert(h2_pal_uart_io_stream_configure(uart, &uc) == H2_PAL_ERR_INVALID_ARG);

  const h2_pal_usb_jtag_io_stream_api_t* usb =
      h2_esp_platform_usb_jtag_io_stream_api();
  const h2_pal_usb_jtag_io_stream_config_t jc = {8192u, 2048u, 1};
  assert(h2_pal_usb_jtag_io_stream_configure(usb, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_usb_jtag_io_stream_configure(usb, &jc) == H2_PAL_ERR_TIMEOUT);
  assert(s_usb_uninstalls == 1u && !s_usb_routes);
  s_install_error = 0;
  assert(h2_pal_usb_jtag_io_stream_configure(usb, &jc) == H2_PAL_OK);
  assert(h2_pal_usb_jtag_io_stream_configure(usb, &jc) == H2_PAL_OK);
  assert(s_usb_installs == 2u && s_usb_routes == 1u && s_usb_uninstalls == 1u);
  assert(h2_pal_usb_jtag_io_stream_write(usb, bytes, sizeof(bytes), &count,
                                         1u) == H2_PAL_OK);
  assert(count == sizeof(bytes));
  assert(h2_pal_usb_jtag_io_stream_read(usb, bytes, sizeof(bytes), &count,
                                        1u) == H2_PAL_OK);
  assert(count == 1u && bytes[0] == 'j');
  assert(h2_pal_usb_jtag_io_stream_flush(usb) == H2_PAL_OK);
  assert(s_usb_installed && s_usb_uninstalls == 1u);
  return 0;
}
