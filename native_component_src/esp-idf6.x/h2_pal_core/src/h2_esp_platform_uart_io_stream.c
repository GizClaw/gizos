#include "h2_esp_platform_core.h"

#include "driver/uart.h"
#include <limits.h>

static h2_pal_result_t configure(void *user, const h2_pal_uart_io_stream_config_t *config) {
    (void)user;
    (void)config;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t read_stream(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    (void)user;
    if (buffer == NULL || out_read == NULL || len > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0u && ticks == 0) {
        ticks = 1;
    }
    int count = uart_read_bytes(
        (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, buffer, (uint32_t)len, ticks);
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

static h2_pal_result_t write_stream(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
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
     * H2Loader installs a ring large enough for one complete framed write, so
     * the following uart_write_bytes copies the whole frame without waiting
     * for ring space while still sharing the driver's console write lock. */
    esp_err_t err = uart_wait_tx_done(
        (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, ticks);
    if (err != ESP_OK) {
        return err == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
    }
    int count = uart_write_bytes(
        (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, buffer, len);
    if (count < 0) {
        return H2_PAL_ERR_IO;
    }
    *out_written = (size_t)count;
    return count == (int)len ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t flush_stream(void *user) {
    (void)user;
    esp_err_t err = uart_wait_tx_done(
        (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(1000u));
    if (err == ESP_OK) {
        return H2_PAL_OK;
    }
    return err == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}

static const h2_pal_uart_io_stream_vtable_t s_vtable = {
    .configure = configure,
    .read = read_stream,
    .write = write_stream,
    .flush = flush_stream,
};

static const h2_pal_uart_io_stream_api_t s_api = {
    .vtable = &s_vtable,
};

const h2_pal_uart_io_stream_api_t *h2_esp_platform_uart_io_stream_api(void) {
    return &s_api;
}
