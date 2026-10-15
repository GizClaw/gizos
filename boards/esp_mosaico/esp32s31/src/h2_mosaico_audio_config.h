#ifndef H2_MOSAICO_AUDIO_CONFIG_H
#define H2_MOSAICO_AUDIO_CONFIG_H
#include "h2_es8311_volume.h"
/* ES8311 register 0x32 has 0.5 dB steps; 0xBF is unity gain.
 * Do not scale its register value linearly by the UI percentage. */
#define H2_MOSAICO_DAC_MAX 0xbfu
#define H2_MOSAICO_VOLUME_CURVE { .point_count = 5, .points = { \
    {1, 120}, {25, 48}, {50, 24}, {75, 12}, {100, 0} } }
#endif
