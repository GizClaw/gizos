#ifndef H2_FRUIT_SLICE_AUDIO_BANK_H
#define H2_FRUIT_SLICE_AUDIO_BANK_H

#include <stddef.h>
#include <stdint.h>
#include "h2_fruit_slice_sound.h"

#define H2_FRUIT_SLICE_AUDIO_BANK_RATE 8000u
#define H2_FRUIT_SLICE_AUDIO_MUSIC H2_FRUIT_SLICE_SOUND_COUNT

typedef struct h2_fruit_slice_audio_clip {
  const int8_t* samples;
  size_t count;
} h2_fruit_slice_audio_clip_t;

extern const h2_fruit_slice_audio_clip_t h2_fruit_slice_audio_bank[];

#endif
