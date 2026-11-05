#include "h2_esp_platform_core.h"
#include "h2/pal/hal/h2_pal_uart_io_stream.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include <limits.h>

static h2_pal_result_t configure(void* user,
                                 const h2_pal_uart_io_stream_config_t* config) {
  (void)user;
  if (config == NULL || config->baud_rate == 0u ||
      config->baud_rate > INT_MAX || config->rx_buffer_size > INT_MAX ||
      config->rx_buffer_size == 0u || config->tx_buffer_size > INT_MAX ||
      config->tx_buffer_size == 0u || config->data_bits < 5u ||
      config->data_bits > 8u ||
      (config->stop_bits != 1u && config->stop_bits != 2u) ||
      config->parity > H2_PAL_UART_PARITY_ODD ||
      (config->flow_control & ~H2_PAL_UART_FLOW_CONTROL_RTS_CTS) != 0u ||
      (config->flags & ~(H2_PAL_UART_IO_STREAM_CONFIG_ROUTE_STDIO |
                         H2_PAL_UART_IO_STREAM_CONFIG_FLUSH_INPUT)) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
  uart_config_t native = {
      .baud_rate = (int)config->baud_rate,
      .data_bits = (uart_word_length_t)(config->data_bits - 5u),
      .parity = config->parity == H2_PAL_UART_PARITY_EVEN ? UART_PARITY_EVEN
                : config->parity == H2_PAL_UART_PARITY_ODD
                    ? UART_PARITY_ODD
                    : UART_PARITY_DISABLE,
      .stop_bits =
          config->stop_bits == 2u ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
      .flow_ctrl = (uart_hw_flowcontrol_t)config->flow_control,
      .source_clk = UART_SCLK_DEFAULT,
  };
  esp_err_t err = uart_driver_install(port, (int)config->rx_buffer_size,
                                      (int)config->tx_buffer_size, 0, NULL, 0);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    err = uart_param_config(port, &native);
  if (err == ESP_OK &&
      (config->flags & H2_PAL_UART_IO_STREAM_CONFIG_ROUTE_STDIO) != 0u)
    uart_vfs_dev_use_driver(port);
  if (err == ESP_OK &&
      (config->flags & H2_PAL_UART_IO_STREAM_CONFIG_FLUSH_INPUT) != 0u)
    err = uart_flush_input(port);
  return err == ESP_OK            ? H2_PAL_OK
         : err == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT
                                  : H2_PAL_ERR_IO;
}

static h2_pal_result_t read_stream(void* user, void* buffer, size_t len,
                                   size_t* out_read, uint32_t timeout_ms) {
  (void)user;
  if (buffer == NULL || out_read == NULL || len > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (timeout_ms != 0u && ticks == 0) {
    ticks = 1;
  }
  int count = uart_read_bytes((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, buffer,
                              (uint32_t)len, ticks);
  *out_read = 0u;
  if (count < 0) {
    return H2_PAL_ERR_IO;
  }
  if (count == 0) {
    return H2_PAL_ERR_TIMEOUT;
  }
  *out_read = (size_t)count;
  return H2_PAL_OK;
}

static h2_pal_result_t write_stream(void* user, const void* buffer, size_t len,
                                    size_t* out_written, uint32_t timeout_ms) {
  (void)user;
  if (buffer == NULL || out_written == NULL || len > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_written = 0u;

  TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (timeout_ms != 0u && ticks == 0) {
    ticks = 1;
  }
  /* Drain the shared console TX ring under the driver's bounded TX mutex.
   * Startup config reserves a ring for one complete framed write, so
   * the following uart_write_bytes copies the whole frame without waiting
   * for ring space while still sharing the driver's console write lock. */
  esp_err_t err =
      uart_wait_tx_done((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, ticks);
  if (err != ESP_OK) {
    return err == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
  }
  int count =
      uart_write_bytes((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, buffer, len);
  if (count < 0) {
    return H2_PAL_ERR_IO;
  }
  *out_written = (size_t)count;
  return count == (int)len ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t flush_stream(void* user) {
  (void)user;
  esp_err_t err = uart_wait_tx_done((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM,
                                    pdMS_TO_TICKS(1000u));
  if (err == ESP_OK) {
    return H2_PAL_OK;
  }
  return err == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}

static h2_pal_result_t release_stream(void* user) {
  (void)user;
  /* The console driver and its stdio route remain shared for boot lifetime. */
  return H2_PAL_OK;
}

static const h2_pal_uart_io_stream_vtable_t s_vtable = {
    .configure = configure,
    .read = read_stream,
    .write = write_stream,
    .flush = flush_stream,
    .release = release_stream,
};

static const h2_pal_uart_io_stream_api_t s_api = {
    .vtable = &s_vtable,
};

const h2_pal_uart_io_stream_api_t* h2_esp_platform_uart_io_stream_api(void) {
  return &s_api;
}
