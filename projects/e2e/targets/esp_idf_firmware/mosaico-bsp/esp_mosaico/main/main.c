#include "h2_chip_board.h"
#include "bsp/esp_mosaico.h"
#include "esp_log.h"

void app_main(void) {
    h2_mosaico_revision_t revision;
    if (h2_mosaico_board_revision(&revision) != 0) {
        ESP_LOGE("mosaico", "Unsupported or unreadable board revision");
        return;
    }
    /* Qualification entry: initialize only the shared bus and power controls.
     * Do not format storage or enable actuators without a dedicated test. */
    esp_err_t rc = bsp_power_init();
    if (rc == ESP_OK) rc = bsp_i2c_init();
    ESP_LOGI("mosaico", "BSP revision=%04x init=%s", (unsigned)revision.version,
             esp_err_to_name(rc));
}
