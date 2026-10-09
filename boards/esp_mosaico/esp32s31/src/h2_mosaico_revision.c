#include "h2_mosaico_revision.h"

#include <stddef.h>

int h2_mosaico_revision_decode(const uint8_t efuse[2],
                              h2_mosaico_revision_t *out_revision) {
    if (out_revision == NULL) {
        return -1;
    }
    *out_revision = (h2_mosaico_revision_t){
        .i2c_sda = -1, .i2c_scl = -1, .lcd_reset = -1, .lcd_clock = -1,
        .codec_power = -1, .status_led = -1, .subboard_i2c_port = -1,
    };
    if (efuse == NULL) {
        return -1;
    }
    const uint16_t version = (uint16_t)((uint16_t)efuse[0] |
                                      ((uint16_t)efuse[1] << 8));
    if (version != 0x0100 && version != 0x0101 && version != 0x0102) {
        return -1;
    }
    const int legacy = version == 0x0100;
    *out_revision = (h2_mosaico_revision_t){
        .version = version,
        .i2c_sda = legacy ? 0 : 56,
        .i2c_scl = legacy ? 1 : 3,
        .lcd_reset = legacy ? 42 : 44,
        .lcd_clock = legacy ? 44 : 42,
        .codec_power = legacy ? 56 : -1,
        .status_led = legacy ? 3 : -1,
        .subboard_i2c_port = legacy ? 0 : 1,
    };
    return 0;
}
