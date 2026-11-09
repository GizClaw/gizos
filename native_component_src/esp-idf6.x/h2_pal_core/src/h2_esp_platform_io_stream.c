#include "sdkconfig.h"
#include "h2_esp_platform_core.h"
#include "h2_pal_unsupported.h"

#ifndef H2_LOADER_SERIAL_RX_BUFFER
#define H2_LOADER_SERIAL_RX_BUFFER 8192u
#endif
#if H2_LOADER_SERIAL_RX_BUFFER < 1024u || H2_LOADER_SERIAL_RX_BUFFER > 8192u
#error "Invalid image-owned console receive budget"
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_UART
/* Serial startup, boot-lifetime driver ownership. Failed setup remains
 * retryable; successful setup never flushes RX again during an app retry. */
static int started;

static h2_pal_result_t stream_start(void* user) {
  (void)user;
  if (started) return H2_PAL_OK;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  const h2_pal_usb_jtag_io_stream_config_t config = {
      .rx_buffer_size = H2_LOADER_SERIAL_RX_BUFFER,
      .tx_buffer_size = 2048u,
      .route_stdio = 1};
  h2_pal_result_t rc = h2_pal_usb_jtag_io_stream_configure(
      h2_esp_platform_usb_jtag_io_stream_api(), &config);
#else
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
      .data_bits = 8u,
      .stop_bits = 1u,
      .rx_buffer_size = H2_LOADER_SERIAL_RX_BUFFER,
      .tx_buffer_size = 2048u,
      .flags = H2_PAL_UART_IO_STREAM_CONFIG_ROUTE_STDIO |
               H2_PAL_UART_IO_STREAM_CONFIG_FLUSH_INPUT,
  };
  h2_pal_result_t rc = h2_pal_uart_io_stream_configure(
      h2_esp_platform_uart_io_stream_api(), &config);
#endif
  if (rc == H2_PAL_OK) started = 1;
  return rc;
}

static h2_pal_result_t stream_read(void* user, void* buffer, size_t len,
                                   size_t* out_read, uint32_t timeout_ms) {
  (void)user;
  if (!started) return H2_PAL_ERR_CLOSED;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  return h2_pal_usb_jtag_io_stream_read(
      h2_esp_platform_usb_jtag_io_stream_api(), buffer, len, out_read,
      timeout_ms);
#else
  return h2_pal_uart_io_stream_read(h2_esp_platform_uart_io_stream_api(),
                                    buffer, len, out_read, timeout_ms);
#endif
}

static h2_pal_result_t stream_write(void* user, const void* buffer, size_t len,
                                    size_t* out_written, uint32_t timeout_ms) {
  (void)user;
  if (!started) return H2_PAL_ERR_CLOSED;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  return h2_pal_usb_jtag_io_stream_write(
      h2_esp_platform_usb_jtag_io_stream_api(), buffer, len, out_written,
      timeout_ms);
#else
  return h2_pal_uart_io_stream_write(h2_esp_platform_uart_io_stream_api(),
                                     buffer, len, out_written, timeout_ms);
#endif
}

static h2_pal_result_t stream_flush(void* user) {
  (void)user;
  if (!started) return H2_PAL_ERR_CLOSED;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  return h2_pal_usb_jtag_io_stream_flush(
      h2_esp_platform_usb_jtag_io_stream_api());
#else
  return h2_pal_uart_io_stream_flush(h2_esp_platform_uart_io_stream_api());
#endif
}

static h2_pal_result_t stream_release(void* user) {
  (void)user;
  /* Shared stdio and driver stay installed for the entire boot. */
  return H2_PAL_OK;
}

static const h2_pal_io_stream_vtable_t vtable = {
    .start = stream_start,
    .read = stream_read,
    .write = stream_write,
    .flush = stream_flush,
    .release = stream_release,
};
static const h2_pal_io_stream_api_t api = {.vtable = &vtable};
#endif

const h2_pal_io_stream_api_t* h2_esp_platform_io_stream_api(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_UART
  return &api;
#else
  return h2_pal_unsupported_io_stream_api();
#endif
}
