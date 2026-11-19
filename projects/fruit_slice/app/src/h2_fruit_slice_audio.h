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

/* The optional worker owns a copy of the mixer and provider handles until
 * stop/join returns them. The controller stays on the App thread. */
struct h2_fruit_slice_audio_worker;

/* Main-loop-owned, bounded mixer. Runtime and APIs are borrowed until close.
 * Optional worker mode transfers mixer ownership through bounded queues.
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
  /* Window counters: producer timing, not a hardware DAC underrun count. */
  uint64_t last_pump_ms;
  uint32_t max_pump_gap_ms, writes, blocked_writes, rebases;
  bool pumped, timeline_started;
  uint32_t music_position;
  uint32_t seen;
  uint8_t mute;
  /* Q16 per-bus gain. Only transitions use the ramp; steady-state PCM is
   * unchanged. A direct/cooperative owner and the PAL worker share this core.
   */
  int32_t sfx_gain, music_gain, sfx_step, music_step;
  uint32_t ramp_left;
  /* Muting waits for accepted PCM and then drain, each with a bounded budget
   * derived from the advertised queue depth. Only the mixer owner uses these.
   */
  uint32_t mute_timeout_ms;
  uint64_t mute_fade_after_ms, mute_drain_after_ms;
  bool closing;
  bool pref_dirty;
  uint64_t pref_retry_ms;
  bool available, speaker_started;
  struct h2_fruit_slice_audio_worker* worker;
  bool worker_requested;
  uint32_t dropped_commands;
} h2_fruit_slice_audio_t;

/* Loads independent fruit_slice/audio_v1 u32 preference. Does not start a
 * speaker; first pump after a user gesture opens the single track. */
int h2_fruit_slice_audio_init(h2_fruit_slice_audio_t* audio,
                              h2_runtime_t* runtime);
/* Open the silent PCM path without mixing/submitting any samples. Native
 * targets may call this during loading to move slow DAC startup off the first
 * gesture. Fully muted audio remains closed. Idempotent; caller owns errors. */
int h2_fruit_slice_audio_prepare(h2_fruit_slice_audio_t* audio);
/* Drop suppressed/burst requests; four fixed voices, priority replacement.
 * now_ms is monotonic wall time. BOMB clears lower-priority voices. */
void h2_fruit_slice_audio_emit(h2_fruit_slice_audio_t* audio,
                               h2_fruit_slice_sound_t sound, uint64_t now_ms);
/* Queue the latest requested mute in worker mode, without joining, touching
 * hardware or saving on the caller. Single-bus changes keep the track and
 * ramp its digital bus over 10 ms. The worker releases hardware after a full
 * mute fade and retries failed control cleanup/settings writes. Queue pressure
 * leaves the previous accepted value so the App retries on its next frame.
 * If optional producer start fails, playback stays disabled and settings use
 * the cooperative save path rather than retrying task allocation each frame.
 * Cooperative/disabled targets persist synchronously, retry dirty settings
 * after 500 ms and make one final best-effort attempt on close. */
int h2_fruit_slice_audio_set_mute(h2_fruit_slice_audio_t* audio, uint8_t mute);
/* At most three native frames per call, at most 20 ms of lookahead, write
 * timeout zero. Rejected frames commit no voice/music/gain progress; subsequent
 * controls regenerate from the last accepted state. Full mute retries the fade
 * and nonblocking drain within separate finite budgets, then releases hardware
 * even if its consumer never progresses. Service at frame boundaries/waits. */
int h2_fruit_slice_audio_pump(h2_fruit_slice_audio_t* audio, uint64_t now_ms);
/* Close track and stop speaker, discard voices. Safe to resume by pumping.
 * Runtime consumes a valid close handle even on error; speaker stop is still
 * attempted. On failure PCM/Runtime borrows and any live speaker remain owned
 * by audio; retry close before freeing the owner. Returns the first error. */
int h2_fruit_slice_audio_suspend(h2_fruit_slice_audio_t* audio);
/* Suspend then free PCM. Retry on error before releasing Runtime/owner. */
int h2_fruit_slice_audio_close(h2_fruit_slice_audio_t* audio);
/* App exit variant: after successful worker/writer/hardware teardown, persist
 * final_mute (0..3) even if its playback command was rejected or never offered.
 * Cleanup failure retains ownership for retry; storage keeps close's bounded
 * best-effort policy. No older writer remains to overwrite this final save. */
int h2_fruit_slice_audio_close_with_mute(h2_fruit_slice_audio_t* audio,
                                         uint8_t final_mute);

#endif
