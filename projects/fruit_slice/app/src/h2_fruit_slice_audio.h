#ifndef H2_FRUIT_SLICE_AUDIO_H
#define H2_FRUIT_SLICE_AUDIO_H

#include "h2_runtime.h"
#include "h2_fruit_slice_sound.h"

#define H2_FRUIT_SLICE_AUDIO_VOICES 4u
#define H2_FRUIT_SLICE_AUDIO_MAX_SAMPLES 2048u

typedef struct h2_fruit_slice_audio_voice {
  uint32_t position;
  uint8_t sound;
  bool active;
} h2_fruit_slice_audio_voice_t;

/* Main-loop-owned, bounded mixer. Runtime and APIs are borrowed until close.
 * No audio worker or shared mutable state; all calls run on the App thread.
 * Unsupported audio is optional and explicitly reported by init/pump.
 */
typedef struct h2_fruit_slice_audio {
  h2_runtime_t* runtime;
  h2_pal_audio_track_t* track;
  h2_audio_pcm_format_t format;
  int16_t* pcm;
  h2_fruit_slice_audio_voice_t voices[H2_FRUIT_SLICE_AUDIO_VOICES];
  uint64_t last_sound_ms[H2_FRUIT_SLICE_SOUND_COUNT];
  uint64_t started_ms, submitted_samples;
  uint32_t music_position;
  uint32_t seen;
  uint8_t mute;
  bool available, speaker_started;
} h2_fruit_slice_audio_t;

/* Loads independent fruit_slice/audio_v1 u32 preference. Does not start a
 * speaker; first pump after a user gesture opens the single track. */
int h2_fruit_slice_audio_init(h2_fruit_slice_audio_t* audio,
                              h2_runtime_t* runtime);
/* Drop suppressed/burst requests; four fixed voices, priority replacement.
 * now_ms is monotonic wall time. BOMB clears lower-priority voices. */
void h2_fruit_slice_audio_emit(h2_fruit_slice_audio_t* audio,
                               h2_fruit_slice_sound_t sound, uint64_t now_ms);
/* Changes mute flags immediately and persists independently of game/profile.
 * Flushes queued PCM so muting cannot leave old audio playing. */
int h2_fruit_slice_audio_set_mute(h2_fruit_slice_audio_t* audio, uint8_t mute);
/* At most three native frames per call, write timeout zero, no backlog replay.
 * Call every game frame after draining the game's sound queue. */
int h2_fruit_slice_audio_pump(h2_fruit_slice_audio_t* audio, uint64_t now_ms);
/* Close track and stop speaker, discard voices. Safe to resume by pumping.
 * On teardown failure borrows remain owned by audio; retry before freeing. */
int h2_fruit_slice_audio_suspend(h2_fruit_slice_audio_t* audio);
/* Suspend then free PCM. Retry on error before releasing Runtime/owner. */
int h2_fruit_slice_audio_close(h2_fruit_slice_audio_t* audio);

#endif
