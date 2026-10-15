#include "h2_mosaico_usb_diagnostic.h"
#include "tinyusb_cdc_acm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static h2_pal_result_t write_diagnostic(void *user, const void *buffer, size_t len,
                                       size_t *out, uint32_t timeout_ms) {
    (void)user;
    if (buffer == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out = 0;
    int64_t start = esp_timer_get_time();
    while (*out < len) {
        *out += tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                    (const uint8_t *)buffer + *out, len - *out);
        esp_err_t rc = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        if (rc != ESP_OK && rc != ESP_ERR_NOT_FINISHED && rc != ESP_ERR_TIMEOUT)
            return H2_PAL_ERR_IO;
        if (*out == len) return H2_PAL_OK;
        if ((esp_timer_get_time() - start) / 1000 >= timeout_ms)
            return H2_PAL_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    return H2_PAL_OK;
}
static h2_pal_result_t flush_diagnostic(void *user) {
    (void)user;
    esp_err_t rc = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0,
                                            pdMS_TO_TICKS(1000));
    if (rc == ESP_OK) return H2_PAL_OK;
    return rc == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}
const h2_pal_uart_io_stream_api_t *h2_mosaico_usb_diagnostic_api(void) {
    static const h2_pal_uart_io_stream_vtable_t vtable = {
        .write = write_diagnostic, .flush = flush_diagnostic,
    };
    static const h2_pal_uart_io_stream_api_t api = {.vtable = &vtable};
    return &api;
}
