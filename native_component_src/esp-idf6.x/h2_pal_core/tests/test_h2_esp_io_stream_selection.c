#include "sdkconfig.h"
#include "h2_esp_platform_core.h"
#include "hal/h2_pal_uart_io_stream.h"
#include "hal/h2_pal_usb_jtag_io_stream.h"
#include "h2_pal_unsupported.h"

#include <assert.h>

#ifndef TEST_SERIAL_RX_BUFFER
#define TEST_SERIAL_RX_BUFFER 8192u
#endif

static unsigned configurations, reads, writes, flushes;
static h2_pal_result_t configure_result = H2_PAL_ERR_NO_MEMORY;
static const char uart_user = 0, usb_user = 0;

static h2_pal_result_t configure_uart(
    void* user, const h2_pal_uart_io_stream_config_t* config) {
  assert(user == &uart_user);
  assert(!CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_ESP_CONSOLE_UART);
  assert(config->baud_rate == 460800u && config->data_bits == 8u &&
         config->stop_bits == 1u && config->parity == H2_PAL_UART_PARITY_NONE &&
         config->flow_control == H2_PAL_UART_FLOW_CONTROL_NONE);
  assert(config->rx_buffer_size == TEST_SERIAL_RX_BUFFER &&
         config->tx_buffer_size == 2048u);
  assert(config->flags == (H2_PAL_UART_IO_STREAM_CONFIG_ROUTE_STDIO |
                           H2_PAL_UART_IO_STREAM_CONFIG_FLUSH_INPUT));
  ++configurations;
  return configure_result;
}
static h2_pal_result_t configure_usb(
    void* user, const h2_pal_usb_jtag_io_stream_config_t* config) {
  assert(user == &usb_user && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG);
  assert(config->rx_buffer_size == TEST_SERIAL_RX_BUFFER &&
         config->tx_buffer_size == 2048u && config->route_stdio == 1);
  ++configurations;
  return configure_result;
}
static void check_user(void* user) {
  assert(user == (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG ? &usb_user : &uart_user));
}
static h2_pal_result_t read_bytes(void* user, void* buffer, size_t len,
                                  size_t* count, uint32_t timeout_ms) {
  check_user(user);
  assert(len == 3u && timeout_ms == 17u);
  ++reads;
  *(char*)buffer = 'r';
  *count = 1u;
  return H2_PAL_OK;
}
static h2_pal_result_t write_bytes(void* user, const void* buffer, size_t len,
                                   size_t* count, uint32_t timeout_ms) {
  check_user(user);
  assert(buffer != NULL && len == 3u && timeout_ms == 23u);
  ++writes;
  *count = 2u;
  return H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t flush(void* user) {
  check_user(user);
  ++flushes;
  return H2_PAL_ERR_IO;
}
static h2_pal_result_t release_uart(void* user) {
  (void)user;
  assert(0); /* The generic provider never tears down the shared console. */
  return H2_PAL_ERR_IO;
}
const h2_pal_uart_io_stream_api_t* h2_esp_platform_uart_io_stream_api(void) {
  static const h2_pal_uart_io_stream_vtable_t vtable = {
      .configure = configure_uart,
      .read = read_bytes,
      .write = write_bytes,
      .flush = flush,
      .release = release_uart,
  };
  static const h2_pal_uart_io_stream_api_t api = {.user = (void*)&uart_user,
                                                  .vtable = &vtable};
  return &api;
}
const h2_pal_usb_jtag_io_stream_api_t* h2_esp_platform_usb_jtag_io_stream_api(
    void) {
  static const h2_pal_usb_jtag_io_stream_vtable_t vtable = {
      .configure = configure_usb,
      .read = read_bytes,
      .write = write_bytes,
      .flush = flush,
  };
  static const h2_pal_usb_jtag_io_stream_api_t api = {.user = (void*)&usb_user,
                                                      .vtable = &vtable};
  return &api;
}

int main(void) {
  const h2_pal_io_stream_api_t* api = h2_esp_platform_io_stream_api();
  char bytes[3] = {0};
  size_t count = 99u;
  assert(configurations == 0u); /* Getter is safe before Runtime init. */
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && !CONFIG_ESP_CONSOLE_UART
  assert(api == h2_pal_unsupported_io_stream_api());
  assert(h2_pal_io_stream_start(api) == H2_PAL_ERR_UNSUPPORTED);
#else
  assert(h2_pal_io_stream_read(api, bytes, sizeof(bytes), &count, 17u) ==
         H2_PAL_ERR_CLOSED);
  assert(count == 0u && reads == 0u);
  assert(h2_pal_io_stream_start(api) == H2_PAL_ERR_NO_MEMORY);
  assert(configurations == 1u);
  configure_result = H2_PAL_OK;
  assert(h2_pal_io_stream_start(api) == H2_PAL_OK);
  assert(h2_pal_io_stream_start(api) == H2_PAL_OK);
  assert(configurations == 2u);
  assert(h2_pal_io_stream_read(api, bytes, sizeof(bytes), &count, 17u) ==
         H2_PAL_OK);
  assert(count == 1u && bytes[0] == 'r' && reads == 1u);
  assert(h2_pal_io_stream_write(api, bytes, sizeof(bytes), &count, 23u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(count == 2u && writes == 1u);
  assert(h2_pal_io_stream_flush(api) == H2_PAL_ERR_IO && flushes == 1u);
  assert(h2_pal_io_stream_release(api) == H2_PAL_OK);
  assert(h2_pal_io_stream_release(api) == H2_PAL_OK);
  assert(h2_pal_io_stream_start(api) == H2_PAL_OK && configurations == 2u);
#endif
  (void)bytes;
  (void)count;
  return 0;
}
