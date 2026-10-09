#include "h2_mosaico_revision.h"

#include <assert.h>
#include <stddef.h>

int main(void) {
    h2_mosaico_revision_t revision;
    assert(h2_mosaico_revision_decode(NULL, &revision) == -1);
    assert(revision.version == 0 && revision.codec_power == -1);
    const uint8_t valid[] = {0, 1};
    assert(h2_mosaico_revision_decode(valid, NULL) == -1);
    for (unsigned int raw = 0; raw <= UINT16_MAX; ++raw) {
        const uint8_t bytes[] = {(uint8_t)raw, (uint8_t)(raw >> 8)};
        const int rc = h2_mosaico_revision_decode(bytes, &revision);
        if (raw == 0x0100) {
            assert(rc == 0 && revision.version == raw);
            assert(revision.i2c_sda == 0 && revision.i2c_scl == 1);
            assert(revision.lcd_reset == 42 && revision.lcd_clock == 44);
            assert(revision.codec_power == 56 && revision.status_led == 3);
            assert(revision.subboard_i2c_port == 0);
        } else if (raw == 0x0101 || raw == 0x0102) {
            assert(rc == 0 && revision.version == raw);
            assert(revision.i2c_sda == 56 && revision.i2c_scl == 3);
            assert(revision.lcd_reset == 44 && revision.lcd_clock == 42);
            assert(revision.codec_power == -1 && revision.status_led == -1);
            assert(revision.subboard_i2c_port == 1);
        } else {
            assert(rc == -1 && revision.version == 0);
            assert(revision.i2c_sda == -1 && revision.i2c_scl == -1);
            assert(revision.lcd_reset == -1 && revision.lcd_clock == -1);
            assert(revision.codec_power == -1 && revision.status_led == -1);
            assert(revision.subboard_i2c_port == -1);
        }
    }
    return 0;
}
