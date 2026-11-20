#ifndef BUFFET_AUDIO_H
#define BUFFET_AUDIO_H
#include "buffet_game.h"

enum {
  BF_MUSIC,
  BF_ADMIT,
  BF_PLATE,
  BF_BITE,
  BF_RED,
  BF_RECOVER,
  BF_FLIP,
  BF_REVEAL,
  BF_CLOSE,
  BF_SERVE,
  BF_SOUND_COUNT
};
typedef struct {
  const int16_t* samples;
  uint32_t count;
} bf_clip_t;
extern const bf_clip_t bf_tracks[BF_SOUND_COUNT];
typedef struct {
  uint64_t pos;
  unsigned cue, delay;
} bf_voice_t;
typedef struct {
  bf_voice_t voices[3];
  uint64_t song_pos;
  uint32_t rate;
  float envelope;
  bool music_on;
} bf_mix_t;
typedef struct {
  bf_phase_t phase;
  int eaten, reveal, queue;
  bool red, muted;
  unsigned red_entries, served;
} bf_sound_state_t;
typedef struct {
  h2_runtime_t* rt;
  const h2_buffet_config_t* config;
  h2_pal_audio_track_t* track;
  h2_audio_pcm_format_t format;
  bf_mix_t mix;
  int16_t* samples;
  bool available, speaker, clock_started;
  uint64_t origin, sent, retry_after, tail_until;
} bf_audio_t;
bf_sound_state_t bf_sound_state(const bf_game_t* g);
void bf_mix_emit(bf_mix_t* m, unsigned cue, unsigned delay_ms);
void bf_mix_frame(bf_mix_t* m, int16_t* output, unsigned frames,
                  unsigned channels);
void bf_audio_init(bf_audio_t* a, h2_runtime_t* rt, bf_game_t* g,
                   const h2_buffet_config_t* cfg);
void bf_audio_update(bf_audio_t* a, bf_sound_state_t before, const bf_game_t* g,
                     uint64_t now, bool inactive);
h2_pal_result_t bf_audio_close(bf_audio_t* a);
#endif
