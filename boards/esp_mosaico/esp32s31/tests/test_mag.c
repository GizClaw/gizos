#include "h2_mosaico_mag.h"
#include <assert.h>

int main(void) {
    uint32_t flags = H2_PAL_IMU_HAS_MAG;
    /* Official no-contact fixture: a saturated left Y does not invalidate X/Z. */
    assert(h2_mosaico_mag_axis(-1198, H2_PAL_IMU_MAG_X_SATURATED, &flags) == -11980);
    assert(h2_mosaico_mag_axis(INT16_MIN, H2_PAL_IMU_MAG_Y_SATURATED, &flags) == H2_PAL_IMU_MAG_INVALID);
    assert(h2_mosaico_mag_axis(484, H2_PAL_IMU_MAG_Z_SATURATED, &flags) == 4840);
    assert(flags == (H2_PAL_IMU_HAS_MAG | H2_PAL_IMU_MAG_Y_SATURATED));
    flags = H2_PAL_IMU_HAS_MAG;
    assert(h2_mosaico_mag_axis(-32767, H2_PAL_IMU_MAG_X_SATURATED, &flags) == -327670);
    assert(h2_mosaico_mag_axis(32767, H2_PAL_IMU_MAG_Z_SATURATED, &flags) == 327670);
    assert(flags == H2_PAL_IMU_HAS_MAG);
    assert(h2_mosaico_mag_axis(INT16_MIN, H2_PAL_IMU_MAG_X_SATURATED, &flags) == H2_PAL_IMU_MAG_INVALID);
    assert(h2_mosaico_mag_axis(INT16_MIN, H2_PAL_IMU_MAG_Z_SATURATED, &flags) == H2_PAL_IMU_MAG_INVALID);
    assert(flags == (H2_PAL_IMU_HAS_MAG | H2_PAL_IMU_MAG_X_SATURATED | H2_PAL_IMU_MAG_Z_SATURATED));
    return 0;
}
