#ifndef H2_MOSAICO_REVISION_H
#define H2_MOSAICO_REVISION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Board wiring selected from the first 16 USER_DATA eFuse bits.
 * GPIO -1 denotes a signal absent on that revision. Values contain no SDK
 * handles and remain valid after the decoding call returns.
 */
typedef struct h2_mosaico_revision {
    uint16_t version;
    int i2c_sda;
    int i2c_scl;
    int lcd_reset;
    int lcd_clock;
    int codec_power;
    int status_led;
    int subboard_i2c_port;
} h2_mosaico_revision_t;

/** Decode little-endian eFuse bytes without touching hardware.
 * Returns 0 for supported v1.0, v1.1 and v1.2; -1 for NULL arguments or an
 * unknown/unprogrammed revision. On failure a non-NULL output is cleared
 * with all GPIO fields set to -1. Thread-safe; does not retain either pointer.
 */
int h2_mosaico_revision_decode(const uint8_t efuse[2],
                              h2_mosaico_revision_t *out_revision);

#ifdef __cplusplus
}
#endif
#endif
