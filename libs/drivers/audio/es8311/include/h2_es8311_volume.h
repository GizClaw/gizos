#ifndef H2_ES8311_VOLUME_H
#define H2_ES8311_VOLUME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ES8311_VOLUME_MAX_POINTS 8u

/** @brief One board-selected attenuation relative to the maximum DAC gain. */
typedef struct h2_es8311_volume_point {
    uint8_t percent; /**< 1..100, strictly increasing. */
    uint8_t attenuation_half_db; /**< Non-increasing, in 0.5 dB steps. */
} h2_es8311_volume_point_t;

/**
 * @brief Optional board curve, copied by value during audio system init.
 *
 * Zero point_count selects the legacy floor(percent * max_register / 100)
 * mapping; unused points are ignored. Otherwise use 2..8 points, starting at
 * 1 percent and ending at 100 percent with zero attenuation. Attenuation must
 * be less than max_register so every nonzero volume has a nonzero DAC value.
 * Segments interpolate register gain linearly, rounding toward lower gain.
 * No pointers are retained; the caller may discard its config after init.
 * Do not modify the initialized system's config while it is in use.
 */
typedef struct h2_es8311_volume_config {
    uint8_t point_count;
    h2_es8311_volume_point_t points[H2_ES8311_VOLUME_MAX_POINTS];
} h2_es8311_volume_config_t;

/** @brief Return 1 for a valid borrowed config and max_register (1..255), else 0.
 * Pure, nonblocking; NULL is invalid. No state is changed.
 */
int h2_es8311_volume_is_valid(const h2_es8311_volume_config_t *config,
                            uint8_t max_register);

/** @brief Convert 0..100 percent to DAC register 0x32.
 * Pure, nonblocking; borrows config for this call. Invalid config returns zero.
 * An enabled curve returns zero for percent > 100; legacy mode preserves the
 * original uint32_t multiply, divide and 0xFF saturation for all inputs.
 * Zero percent always returns zero. With an enabled curve the driver also
 * asserts DAC mute at zero; legacy mute behavior is unchanged.
 * max_register is the board's codec_volume_default: 0xBF = 0 dB,
 * one register step = 0.5 dB, range 1..255 (-95..+32 dB).
 */
uint8_t h2_es8311_volume_from_percent(const h2_es8311_volume_config_t *config,
                                    uint8_t max_register, uint32_t percent);

#ifdef __cplusplus
}
#endif
#endif
