#include "h2_es8311_power.h"

#include <stddef.h>

int h2_es8311_suspend(void *user, h2_es8311_write_reg_fn write_reg) {
    /* Espressif esp_codec_dev es8311_suspend, esp-adf release/v2.x.
     * Repeated clock/reset writes are part of the ordered hardware sequence. */
    static const uint8_t suspend_registers[][2] = {
        {0x32, 0x00}, {0x17, 0x00}, {0x0e, 0xff}, {0x12, 0x02},
        {0x14, 0x00}, {0x0d, 0xfa}, {0x15, 0x00}, {0x02, 0x10},
        {0x00, 0x00}, {0x00, 0x1f}, {0x01, 0x30}, {0x01, 0x00},
        {0x45, 0x00}, {0x0d, 0xfc}, {0x02, 0x00},
    };
    if (write_reg == NULL) {
        return -1;
    }
    int first_rc = 0;
    for (size_t i = 0; i < sizeof(suspend_registers) / sizeof(suspend_registers[0]); ++i) {
        int rc = write_reg(user, suspend_registers[i][0], suspend_registers[i][1]);
        if (first_rc == 0) {
            first_rc = rc;
        }
    }
    return first_rc;
}
