#ifndef H2_GARDEN_GUARD_AUDIO_H
#define H2_GARDEN_GUARD_AUDIO_H

#include "h2_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define H2_GARDEN_GUARD_AUDIO_TASK "garden-guard/audio"

typedef enum gg_sound {
  GG_SOUND_TAP,
  GG_SOUND_DENIED,
  GG_SOUND_SUN,
  GG_SOUND_PLANT,
  GG_SOUND_SHOT,
  GG_SOUND_HIT,
  GG_SOUND_CART,
  GG_SOUND_WON,
  GG_SOUND_LOST,
  GG_SOUND_COUNT,
} gg_sound_t;

typedef struct gg_audio gg_audio_t;

/* Only the run thread starts, publishes, and joins. The worker alone opens
 * and services the PAL stream. A NULL worker is a supported silent session. */
gg_audio_t* gg_audio_start(h2_runtime_t* runtime);
void gg_audio_publish(gg_audio_t* audio, uint16_t sounds, bool music,
                      bool muted, bool reset);
int gg_audio_join(gg_audio_t* audio);

#endif
