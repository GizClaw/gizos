#include "h2_mosaico_audio_config.h"
#include <assert.h>
int main(void) {
    const h2_es8311_volume_config_t curve = H2_MOSAICO_VOLUME_CURVE;
    assert(h2_es8311_volume_is_valid(&curve, H2_MOSAICO_DAC_MAX));
    assert(h2_es8311_volume_from_percent(&curve, H2_MOSAICO_DAC_MAX, 0) == 0);
    /* Ordinary listening volume must not recreate the legacy -57.5 dB at 40%. */
    const unsigned normal = h2_es8311_volume_from_percent(&curve, H2_MOSAICO_DAC_MAX, 40);
    assert(normal >= H2_MOSAICO_DAC_MAX - 40 && normal <= H2_MOSAICO_DAC_MAX - 24);
    unsigned previous = 0;
    for (unsigned p = 1; p <= 100; ++p) {
        const unsigned value = h2_es8311_volume_from_percent(&curve, H2_MOSAICO_DAC_MAX, p);
        assert(value >= previous && value <= H2_MOSAICO_DAC_MAX);
        previous = value;
    }
    assert(previous == H2_MOSAICO_DAC_MAX);
    return 0;
}
