#ifndef H2_ESP_ES8311_ES7210_AEC_STATS_H
#define H2_ESP_ES8311_ES7210_AEC_STATS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum h2_esp_es8311_es7210_aec_nlp_level {
    H2_ESP_ES8311_ES7210_AEC_NLP_NORMAL = 0,
    H2_ESP_ES8311_ES7210_AEC_NLP_AGGRESSIVE = 1,
} h2_esp_es8311_es7210_aec_nlp_level_t;

/** Complete-frame mean square, peak and clipped-sample counts. No PCM or
 * identity is retained. Lanes describe the actual raw I2S buffer order, which
 * can differ from ADC input numbers. Powers use unscaled raw reference. */
typedef struct h2_esp_es8311_es7210_aec_stats {
    uint32_t frame;
    uint16_t samples;
    uint8_t raw_channels;
    uint8_t mic_lane;
    uint8_t ref_lane;
    uint32_t lane_power[4];
    uint32_t lane_peak[4];
    uint32_t lane_clipped[4];
    uint32_t output_power;
    uint32_t output_peak;
    uint32_t output_clipped;
} h2_esp_es8311_es7210_aec_stats_t;

/** Optional synchronous observer on the mic processing task. Stats are
 * borrowed only during the callback. Do not wait, allocate, call the Audio
 * lifecycle or reenter processing. Caller synchronizes any retained copy and
 * keeps user alive through successful Audio System deinit. */
typedef void (*h2_esp_es8311_es7210_aec_observe_fn)(
    void *user, const h2_esp_es8311_es7210_aec_stats_t *stats);

/** Optional bounded per-frame request check on the same mic task and user.
 * False skips diagnostic energy calculation and delivery, without altering
 * AEC processing. NULL requests every frame whenever observe is installed. */
typedef bool (*h2_esp_es8311_es7210_aec_observe_enabled_fn)(void *user);

#endif
