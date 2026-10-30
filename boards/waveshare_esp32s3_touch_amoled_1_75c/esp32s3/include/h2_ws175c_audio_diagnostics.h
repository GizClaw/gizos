#ifndef H2_WS175C_AUDIO_DIAGNOSTICS_H
#define H2_WS175C_AUDIO_DIAGNOSTICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_ws175c_audio_raw_stats {
  uint64_t samples_per_lane;
  uint32_t peak[4];
  uint64_t energy[4];
  uint64_t data_high_samples;
  uint64_t data_level_samples;
} h2_ws175c_audio_raw_stats_t;

/* Serialized board bring-up diagnostic. Requires no active mic or speaker.
 * Owns a bounded speaker-clock session, reads the real four ADC lanes, then
 * stops the session even on error. Retains only aggregate statistics.
 * This SDK diagnostic is independent of PAL Audio qualification and AEC. */
int h2_ws175c_audio_probe_raw(uint32_t duration_ms,
                              h2_ws175c_audio_raw_stats_t* out_stats);

/* Select a real direct-AEC profile for the single-board bring-up program.
 * Only before Audio initialization, or after successful stopped/joined deinit.
 * Does not persist settings or change the normal board default. The reference
 * PGA stays independent of subsequent PAL microphone-gain changes. */
int h2_ws175c_audio_probe_select_aec(uint8_t microphones,
                                     uint8_t reference_gain_db);
/* Stop/join first; failed release keeps the diagnostic profile and providers.
 */
int h2_ws175c_audio_probe_release(void);

typedef struct h2_ws175c_audio_controls {
  uint8_t dac_register;
  uint8_t adc_gain_registers[4];
} h2_ws175c_audio_controls_t;

/* Read actual codec registers after initialization with both workers stopped
 * and joined. No controls, clocks, power or gain values are changed. */
int h2_ws175c_audio_probe_read_controls(h2_ws175c_audio_controls_t* out);

#ifdef __cplusplus
}
#endif

#endif
