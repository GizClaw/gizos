#include "h2_mosaico_usb_console.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"

int h2_mosaico_usb_console_init(void) {
    const tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    int rc = tinyusb_driver_install(&config);
    if (rc != ESP_OK) return rc;
    const tinyusb_config_cdcacm_t cdc = {0};
    rc = tinyusb_cdcacm_init(&cdc);
    if (rc != ESP_OK) return rc;
    return tinyusb_console_init(TINYUSB_CDC_ACM_0);
}
