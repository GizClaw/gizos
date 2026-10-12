#include "audible.h"
#include <math.h>
#include <string.h>

int h2_mosaico_audible_cue(const h2_pal_audio_api_t *audio,
                           h2_mosaico_audible_result_t *out) {
  if (!out)
    return H2_AUDIO_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_audio_info_t info = {0};
  int rc = h2_pal_audio_get_info(audio, &info);
  if (rc != 0)
    return out->work_rc = rc;
  const h2_audio_pcm_format_t f = info.playback_format;
  if (f.sample_format != H2_AUDIO_SAMPLE_S16LE || f.sample_rate_hz != 16000u ||
      f.channels == 0u || f.channels > 2u ||
      f.frame_samples_per_channel == 0u || f.frame_samples_per_channel > 512u)
    return out->work_rc = H2_AUDIO_ERR_UNSUPPORTED;
  rc = h2_pal_audio_get_speaker_volume_percent(audio, &out->saved_volume);
  if (rc != 0)
    return out->work_rc = rc;
  if (out->saved_volume > 100u)
    return out->work_rc = H2_AUDIO_ERR_INVALID_STATE;
  h2_pal_audio_track_t *track = NULL;
  int started = 0;
  rc = h2_pal_audio_set_speaker_volume_percent(audio, 100u);
  if (rc == 0)
    rc = h2_pal_audio_get_speaker_volume_percent(audio, &out->applied_volume);
  if (rc == 0 && out->applied_volume != 100u)
    rc = H2_AUDIO_ERR_INVALID_STATE;
  if (rc == 0) {
    rc = h2_pal_audio_start_speaker(audio);
    started = rc == 0;
  }
  const h2_audio_track_config_t config = {.name = "mosaico-audible",
                                          .format = f,
                                          .volume_factor_milli = 1000u,
                                          .buffer_frames = 4u};
  if (rc == 0)
    rc = h2_pal_audio_create_track(audio, &config, &track);
  if (rc == 0 && track == NULL)
    rc = H2_AUDIO_ERR_INVALID_STATE;
  int16_t pcm[1024];
  h2_audio_frame_t frame = {.data = pcm,
                            .capacity = sizeof(pcm),
                            .bytes = f.frame_samples_per_channel * f.channels *
                                     sizeof(*pcm),
                            .sample_rate_hz = f.sample_rate_hz,
                            .samples_per_channel = f.frame_samples_per_channel,
                            .channels = f.channels,
                            .sample_format = f.sample_format};
  for (unsigned beep = 0; rc == 0 && beep < 3u; ++beep) {
    for (unsigned block = 0; rc == 0 && block < 20u; ++block) {
      for (unsigned i = 0; i < f.frame_samples_per_channel; ++i) {
        unsigned n = block * f.frame_samples_per_channel + i;
        int16_t sample = block < 12u
                             ? (int16_t)(4096.0f * sinf(6.28318530718f *
                                                        (600u + 400u * beep) *
                                                        n / f.sample_rate_hz))
                             : 0;
        for (unsigned ch = 0; ch < f.channels; ++ch)
          pcm[i * f.channels + ch] = sample;
      }
      rc = h2_pal_audio_track_write(track, &frame, 1000u);
      if (rc == 0)
        ++out->blocks_written;
    }
  }
  if (rc == 0)
    rc = h2_pal_audio_track_drain(track, 3000u);
  out->work_rc = rc;
  if (track != NULL)
    out->close_rc = h2_pal_audio_track_close(track);
  /* A failed close retains provider ownership. The caller must enter its
   * recovery path, not stop/reuse the speaker or continue playing another cue.
   */
  if (started && out->close_rc == 0)
    out->stop_rc = h2_pal_audio_stop_speaker(audio);
  out->restore_rc =
      h2_pal_audio_set_speaker_volume_percent(audio, out->saved_volume);
  return rc != 0              ? rc
         : out->close_rc != 0 ? out->close_rc
         : out->stop_rc != 0  ? out->stop_rc
                              : out->restore_rc;
}
