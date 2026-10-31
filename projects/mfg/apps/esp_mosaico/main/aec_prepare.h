#ifndef MOSAICO_MFG_AEC_PREPARE_H
#define MOSAICO_MFG_AEC_PREPARE_H

#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_time.h"

/* App-only settling before the shared check, never an acceptance result.
 * The shared raw QUIET/FAR/QUIET AGAIN windows still run in full afterwards. */
static void mosaico_prepare_capture(void* user,
                                    const h2_audio_aec_frame_t* frame) {
  (void)user;
  (void)frame;
}

static void mosaico_prepare_playback(void* user,
                                     const h2_audio_frame_t* frame) {
  (void)user;
  (void)frame;
}
typedef struct mosaico_aec_preparation {
  /* Only the mic worker writes. Read after successful stop/join. This storage
   * must outlive failed cleanup and any observer that remains registered. */
  int capture_error;
} mosaico_aec_preparation_t;

static void mosaico_prepare_error(void* user, int capture, int result) {
  if (capture)
    ((mosaico_aec_preparation_t*)user)->capture_error =
        result ? result : H2_PAL_ERR_IO;
}

static int mosaico_aec_prepare(const h2_pal_audio_api_t* audio,
                               const h2_pal_time_api_t* time,
                               mosaico_aec_preparation_t* preparation,
                               uint32_t* windows_out, uint32_t* last_mse_out) {
  *windows_out = *last_mse_out = 0;
  uint64_t started = 0, now = 0;
  int rc = h2_pal_time_get_monotonic_ms(time, &started);
  if (rc != 0) return rc;
  preparation->capture_error = 0;
  const h2_audio_aec_observer_t observer = {
      .user = preparation,
      .on_capture = mosaico_prepare_capture,
      .on_playback = mosaico_prepare_playback,
      .on_error = mosaico_prepare_error};
  rc = h2_pal_audio_set_aec_observer(audio, &observer);
  if (rc != 0) return rc;
  rc = h2_pal_audio_set_speaker_volume_percent(audio, 0);
  if (rc == 0) rc = h2_pal_audio_start_mic(audio);
  int16_t pcm[512];
  uint64_t energy = 0;
  uint32_t frames = 0, clipped = 0, recent[4] = {0};
  int settled = 0;
  for (unsigned reads = 0; rc == 0 && reads < 400; ++reads) {
    rc = h2_pal_time_get_monotonic_ms(time, &now);
    if (rc != 0) break;
    if (now - started >= 8000) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    h2_audio_frame_t frame = {.data = pcm,
                              .capacity = sizeof(pcm),
                              .sample_rate_hz = 16000,
                              .samples_per_channel = 512,
                              .channels = 1,
                              .sample_format = H2_AUDIO_SAMPLE_S16LE};
    rc = h2_pal_audio_mic_read(audio, &frame, 100);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
      rc = 0;
      continue;
    }
    if (rc != 0) break;
    if (frame.bytes != sizeof(pcm) || frame.samples_per_channel != 512 ||
        frame.channels != 1 || frame.sample_rate_hz != 16000 ||
        frame.sample_format != H2_AUDIO_SAMPLE_S16LE) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    for (unsigned i = 0; i < 512; ++i) {
      int32_t value = pcm[i];
      uint32_t magnitude = value < 0 ? (uint32_t)-value : (uint32_t)value;
      energy += (uint64_t)magnitude * magnitude;
      clipped += magnitude >= 30000;
    }
    if (++frames != 16) continue;
    uint32_t mse = (uint32_t)(energy / (16u * 512u));
    if (clipped) {
      *windows_out = 0;
    } else {
      recent[*windows_out % 4] = mse;
      ++*windows_out;
    }
    *last_mse_out = mse;
    frames = clipped = 0;
    energy = 0;
    if (*windows_out < 4 || now - started < 2000) continue;
    uint32_t low = recent[0], high = recent[0];
    for (unsigned i = 1; i < 4; ++i) {
      if (recent[i] < low) low = recent[i];
      if (recent[i] > high) high = recent[i];
    }
    if (high <= (uint64_t)(low ? low : 1) * 4) {
      settled = 1;
      break;
    }
  }
  if (rc == 0 && !settled) rc = H2_PAL_ERR_TIMEOUT;
  /* Always join before unregistering, including start/read failures. */
  int cleanup = h2_pal_audio_stop_mic(audio);
  if (cleanup != 0) return cleanup;
  if (preparation->capture_error != 0) rc = preparation->capture_error;
  cleanup = h2_pal_audio_set_aec_observer(audio, NULL);
  return cleanup != 0 ? cleanup : rc;
}

#endif
