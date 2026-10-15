#ifndef H2_MOSAICO_MAG_H
#define H2_MOSAICO_MAG_H

#include "h2/pal/hal/h2_pal_imu.h"

/* Like the official magnetic interaction example, preserve successful reads
 * with saturated axes. Never convert Bosch's overflow sentinel to milligauss. */
static inline int32_t h2_mosaico_mag_axis(int16_t value, uint32_t saturation_flag,
                                        uint32_t *flags) {
    if (value == INT16_MIN) {
        *flags |= saturation_flag;
        return H2_PAL_IMU_MAG_INVALID;
    }
    return (int32_t)value * 10;
}

#endif
