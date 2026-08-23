#include "h2_esp_platform_core.h"

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

#include <limits.h>

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
    int count = usb_serial_jtag_read_bytes(buffer, len, ticks);
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
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0u && ticks == 0) {
        ticks = 1;
    }
    int count = usb_serial_jtag_write_bytes(buffer, len, ticks);
    if (count < 0) {
        *out_written = 0u;
        return H2_PAL_ERR_IO;
    }
    *out_written = (size_t)count;
    if (count == 0 && len > 0u) {
        return H2_PAL_ERR_TIMEOUT;
    }
    return count == (int)len ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t flush_stream(void *user) {
    (void)user;
    esp_err_t err = usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000u));
    if (err == ESP_OK) {
        return H2_PAL_OK;
    }
    return err == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}

static const h2_pal_usb_jtag_io_stream_vtable_t s_vtable = {
    .read = read_stream,
    .write = write_stream,
    .flush = flush_stream,
};

static const h2_pal_usb_jtag_io_stream_api_t s_api = {
    .vtable = &s_vtable,
};

const h2_pal_usb_jtag_io_stream_api_t *h2_esp_platform_usb_jtag_io_stream_api(void) {
    return &s_api;
}
