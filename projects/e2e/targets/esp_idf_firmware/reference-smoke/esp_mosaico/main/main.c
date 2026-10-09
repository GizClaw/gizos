#include "h2_chip_board.h"
#include "h2_reference_smoke.h"

#include "esp_log.h"

#include <stdint.h>

volatile uint32_t h2_esp_chip_reference_result;

void app_main(void) {
    h2_mosaico_revision_t revision;
    if (h2_mosaico_board_revision(&revision) != 0) {
        ESP_LOGE("mosaico", "Unsupported or unreadable eFuse hardware revision");
        return;
    }
    ESP_LOGI("mosaico", "%s revision=%04x SDA=%d SCL=%d", h2_chip_board_name(),
             (unsigned)revision.version, revision.i2c_sda, revision.i2c_scl);
    h2_esp_chip_reference_result = h2_reference_smoke_value(UINT32_C(32));
}
