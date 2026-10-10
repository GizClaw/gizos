#include "h2_mosaico_loader_usb.h"
#include "h2_mosaico_usb_console.h"
#include "h2_esp_h2loader_iostreamikcp.h"
#include "tinyusb_cdc_acm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_TINYUSB_CDC_COUNT != 2
#error "Mosaico Loader requires separate diagnostic and command CDC interfaces"
#endif

static int64_t elapsed_ms(int64_t start) {
    return (esp_timer_get_time() - start) / 1000;
}
static h2_pal_result_t read_command(void *user, void *buffer, size_t len,
                                   size_t *out, uint32_t timeout_ms) {
    (void)user;
    if (buffer == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out = 0;
    if (len == 0) return H2_PAL_OK;
    int64_t start = esp_timer_get_time();
    for (;;) {
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_1, buffer, len, out) != ESP_OK)
            return H2_PAL_ERR_IO;
        if (*out != 0) return H2_PAL_OK;
        if (elapsed_ms(start) >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
static h2_pal_result_t write_command(void *user, const void *buffer, size_t len,
                                    size_t *out, uint32_t timeout_ms) {
    (void)user;
    if (buffer == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out = 0;
    int64_t start = esp_timer_get_time();
    while (*out < len) {
        *out += tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_1,
                    (const uint8_t *)buffer + *out, len - *out);
        esp_err_t rc = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_1, 0);
        if (rc != ESP_OK && rc != ESP_ERR_NOT_FINISHED && rc != ESP_ERR_TIMEOUT) return H2_PAL_ERR_IO;
        if (*out == len) return H2_PAL_OK;
        if (elapsed_ms(start) >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    return H2_PAL_OK;
}
static h2_pal_result_t flush_command(void *user) {
    (void)user;
    esp_err_t rc = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_1,
                                            pdMS_TO_TICKS(1000));
    if (rc == ESP_OK) return H2_PAL_OK;
    return rc == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}
int h2_mosaico_loader_usb_init(void) {
    int rc = h2_mosaico_usb_console_init();
    if (rc != ESP_OK) return H2_PAL_ERR_IO;
    const tinyusb_config_cdcacm_t config = {.cdc_port = TINYUSB_CDC_ACM_1};
    if (tinyusb_cdcacm_init(&config) != ESP_OK) return H2_PAL_ERR_IO;
    const h2_iostreamikcp_io_t io = {
        .read = read_command, .write = write_command, .flush = flush_command,
    };
    return h2_esp_h2loader_configure_physical_io(&io);
}
