#ifndef H2_GARDEN_GUARD_SFX_H
#define H2_GARDEN_GUARD_SFX_H

#include <stdbool.h>
#include <stdint.h>

/* Garden Guard sound effects: an original set of short cues rendered from a
 * compact step table by a fixed-point synthesizer. No PCM bank, no decoder.
 *
 * Cue bits follow gg_sound_t in h2_garden_guard_audio.h; the score asserts
 * that both orders match. The run thread only ORs bits into one word. The
 * audio worker calls gg_sfx_events() once per block and gg_sfx_next() for
 * every output frame, so work per frame is bounded by GG_SFX_VOICES. */
enum {
  GG_SFX_TAP,
  GG_SFX_DENIED,
  GG_SFX_SUN,
  GG_SFX_PLANT,
  GG_SFX_SHOT,
  GG_SFX_HIT,
  GG_SFX_CART,
  GG_SFX_WON,
  GG_SFX_LOST,
  GG_SFX_COUNT,
};

/* Voices 0-1 serve the interface lane, 2 the shot lane and 3-4 the impact
 * lane. A fanfare clears the others and may use every voice. */
#define GG_SFX_VOICES 5u
#define GG_SFX_LANES 3u

/* Authored bounds of |gg_sfx_next()|, as sums of the largest amplitude each
 * voice can hold. Interface and combat lanes may sound together; fanfares
 * sound alone, while the music is stopped. Tests recompute them. */
#define GG_SFX_PEAK_UI 7300
#define GG_SFX_PEAK_COMBAT 6900
#define GG_SFX_PEAK_FANFARE 9600

typedef struct gg_sfx_voice {
  uint32_t phase, phase2, step;
  int32_t sweep;       /* Step change per frame. */
  int32_t level, fall; /* Q24 envelope and its fall per frame. */
  int32_t rise;        /* Q15 attack; 32768 once complete. */
  int32_t low, low2;   /* Two-pole noise state. */
  int32_t last;        /* Last output, faded out if the voice is cut. */
  uint16_t amp;
  uint8_t shape, curve;
} gg_sfx_voice_t;

typedef struct gg_sfx_lane {
  const uint8_t* next; /* Next step of the running cue, or NULL. */
  uint32_t wait;       /* Frames until that step. */
  uint32_t started;    /* Clock when the running cue began. */
  int8_t transpose;
  uint8_t cue; /* Last cue started + 1, or 0. */
} gg_sfx_lane_t;

typedef struct gg_sfx {
  gg_sfx_voice_t voice[GG_SFX_VOICES];
  gg_sfx_lane_t lane[GG_SFX_LANES];
  uint32_t rate, tick, lift, noise, clock, sun_at;
  int32_t residual;
  uint8_t sun_run, shot_turn, fade;
} gg_sfx_t;

/* rate is 8000..48000 Hz. */
void gg_sfx_init(gg_sfx_t* sfx, uint32_t rate);
/* Starts cues from a coalesced bit set; at most one per lane. */
void gg_sfx_events(gg_sfx_t* sfx, uint16_t events);
void gg_sfx_clear(gg_sfx_t* sfx);
bool gg_sfx_active(const gg_sfx_t* sfx);
/* True while a win or loss fanfare is sounding. */
bool gg_sfx_fanfare(const gg_sfx_t* sfx);
/* Next mono frame. Call it for every frame the worker renders, also while
 * idle, so that the retrigger clock keeps time. */
int gg_sfx_next(gg_sfx_t* sfx);

#endif
