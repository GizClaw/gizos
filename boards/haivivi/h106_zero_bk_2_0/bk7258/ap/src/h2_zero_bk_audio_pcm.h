#ifndef H2_ZERO_BK_AUDIO_PCM_H
#define H2_ZERO_BK_AUDIO_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The board's original mono input is ADC L. The other FIFO lane is not a
 * second product microphone; keep it out of the mono signal and its gain. */
static inline bool h2_zero_bk_audio_extract_primary(const int16_t* raw,
                                                    size_t raw_samples,
                                                    uint8_t raw_channels,
                                                    int16_t* mono,
                                                    size_t frames) {
  if (raw == NULL || mono == NULL || raw_channels == 0u || raw_channels > 2u ||
      frames > raw_samples / raw_channels)
    return false;
  for (size_t i = 0u; i < frames; ++i) mono[i] = raw[i * raw_channels];
  return true;
}

#endif
