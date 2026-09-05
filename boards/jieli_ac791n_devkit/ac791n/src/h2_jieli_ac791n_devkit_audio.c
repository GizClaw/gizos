#include "app_config.h"

#if defined(CONFIG_AUDIO_ENABLE) && CONFIG_AUDIO_ENABLE

#include "generic/circular_buf.h"
#include "os/os_api.h"
#include "system/sys_time.h"
#include "audio_music/pcm_play_api.h"
#include "server/audio_server.h"
#include "server/server_core.h"

#include "h2_jieli_ac791n_devkit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
  H2_AUDIO_SAMPLE_RATE = 16000,
  H2_AUDIO_FRAME_SAMPLES = 320,
  H2_AUDIO_FRAME_BYTES = H2_AUDIO_FRAME_SAMPLES * sizeof(int16_t),
  H2_AUDIO_MIC_QUEUE_FRAMES = 4,
  H2_AUDIO_TRACK_QUEUE_FRAMES = 4,
  H2_AUDIO_MIC_BUFFER_BYTES =
      H2_AUDIO_MIC_QUEUE_FRAMES * H2_AUDIO_FRAME_BYTES,
  H2_AUDIO_MAX_TRACKS = 2,
  /* The DevKit's post-AEC near-end PCM is intentionally conservative.  Raise
   * only the microphone monitor track so speech remains audible beside the
   * music track; saturating arithmetic prevents wraparound on loud input. */
  H2_AUDIO_MIC_MONITOR_GAIN = 4,
};

typedef struct jieli_audio_track {
  h2_pal_audio_track_t pal;
  void *speaker_pcm;
  const char *name;
  int active;
  volatile int closed;
  volatile uint32_t completed_writes;
  uint32_t volume_factor_milli;
  int16_t mic_monitor_frame[H2_AUDIO_FRAME_SAMPLES];
  uint32_t rate_started_ms;
  uint32_t rate_written_bytes;
  uint32_t rate_produced_bytes;
  uint32_t rate_produced_callbacks;
} jieli_audio_track_t;

typedef struct jieli_audio_state {
  struct server *mic_server;
  cbuffer_t mic_pcm;
  uint8_t mic_storage[H2_AUDIO_MIC_BUFFER_BYTES];
  OS_SEM mic_ready;
  int mic_sem_ready;
  int mic_started;
  volatile uint32_t mic_produced_bytes;
  volatile uint32_t mic_produced_callbacks;
  int speaker_started;
  uint32_t speaker_volume_percent;
  jieli_audio_track_t tracks[H2_AUDIO_MAX_TRACKS];
} jieli_audio_state_t;

static jieli_audio_state_t audio_state = {.speaker_volume_percent = 80u};

static void audio_stage(const char *message) {
  (void)h2_jieli_ac791n_devkit_console_write(
      message, strlen(message), 100u);
}

static uint32_t timeout_ticks(uint32_t timeout_ms) {
  if (timeout_ms == 0u) return 0u;
  uint32_t ticks = (timeout_ms + 9u) / 10u;
  return ticks == 0u ? 1u : ticks;
}

static int mic_vfs_write(void *file, void *data, uint32_t length) {
  (void)file;
  if (length > sizeof(audio_state.mic_storage)) return 0;

  /* The encoder starts before the application consumer task.  Treat this as
   * a realtime "latest frame" queue, matching the ESP and BK audio PALs: an
   * overrun drops stale microphone samples instead of returning a short write
   * that makes JieLi's AEC encoder stop delivering data. */
  if (!cbuf_is_write_able(&audio_state.mic_pcm, length)) {
    cbuf_clear(&audio_state.mic_pcm);
  }
  uint32_t written = cbuf_write(&audio_state.mic_pcm, data, length);
  if (written != length) {
    cbuf_clear(&audio_state.mic_pcm);
    written = cbuf_write(&audio_state.mic_pcm, data, length);
  }
  if (written == length) os_sem_post(&audio_state.mic_ready);
  if (written == length) {
    audio_state.mic_produced_bytes += length;
    ++audio_state.mic_produced_callbacks;
  }
  return written == length ? (int)length : 0;
}

static int audio_vfs_close(void *file) {
  (void)file;
  return 0;
}

static int audio_vfs_length(void *file) {
  (void)file;
  return 0;
}

static const struct audio_vfs_ops mic_vfs_ops = {
    .fwrite = mic_vfs_write,
    .fclose = audio_vfs_close,
    .flen = audio_vfs_length,
};

static int audio_get_info(void *user, h2_audio_info_t *info) {
  (void)user;
  if (info == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  const h2_audio_pcm_format_t format = {
      .sample_rate_hz = H2_AUDIO_SAMPLE_RATE,
      .frame_samples_per_channel = H2_AUDIO_FRAME_SAMPLES,
      .channels = 1u,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
  };
  *info = (h2_audio_info_t){
      .available = 1,
      .mic_supported = 1,
      .playback_supported = 1,
      .mic_format = format,
      .playback_format = format,
      .mic_queue_frames = H2_AUDIO_MIC_QUEUE_FRAMES,
      .track_queue_frames = H2_AUDIO_TRACK_QUEUE_FRAMES,
      .max_tracks = H2_AUDIO_MAX_TRACKS,
  };
  return H2_AUDIO_OK;
}

static int audio_start_mic(void *user) {
  (void)user;
  if (audio_state.mic_started) return H2_AUDIO_OK;
  if (!audio_state.mic_sem_ready) {
    if (os_sem_create(&audio_state.mic_ready, 0) != OS_NO_ERR) {
      return H2_AUDIO_ERR_NO_MEMORY;
    }
    audio_state.mic_sem_ready = 1;
  }
  cbuf_init(
      &audio_state.mic_pcm, audio_state.mic_storage,
      sizeof(audio_state.mic_storage));
  os_sem_set(&audio_state.mic_ready, 0);
  audio_state.mic_produced_bytes = 0u;
  audio_state.mic_produced_callbacks = 0u;
  audio_state.mic_server = server_open("audio_server", "enc");
  if (audio_state.mic_server == NULL) return H2_AUDIO_ERR_UNAVAILABLE;
  union audio_req request;
  memset(&request, 0, sizeof(request));
  request.enc.cmd = AUDIO_ENC_OPEN;
  request.enc.channel = 1u;
  /* The board platform opens only MIC1 rather than the SDK's four-channel
   * multiplex mode.  JieLi's own recorder/UAC paths use a zero bitmap in this
   * topology; BIT(physical_channel) is valid only with
   * CONFIG_ALL_ADC_CHANNEL_OPEN_ENABLE and otherwise strides the input as a
   * multiplexed stream. */
  request.enc.channel_bit_map = 0u;
  request.enc.volume = CONFIG_AUDIO_ADC_GAIN;
  request.enc.output_buf_len = 8192u;
  request.enc.sample_rate = H2_AUDIO_SAMPLE_RATE;
  request.enc.format = "pcm";
  request.enc.frame_size = H2_AUDIO_FRAME_SAMPLES * sizeof(int16_t);
  request.enc.sample_source = "mic";
  request.enc.vfs_ops = &mic_vfs_ops;
  request.enc.file = (FILE *)&audio_state.mic_pcm;
#if defined(CONFIG_AEC_ENC_ENABLE)
  /* Use the PCM stream sent to the on-chip DAC as the far-end reference.
   * This board exposes one ADC microphone channel and has no separate analog
   * feedback channel, so JieLi's software-DAC reference path is the correct
   * topology.  audio_server owns the AEC/NLP/ANS processing and delivers the
   * processed near-end PCM to mic_vfs_write(). */
  struct aec_s_attr aec_param;
  memset(&aec_param, 0, sizeof(aec_param));
  extern void get_cfg_file_aec_config(struct aec_s_attr *aec_param);
  get_cfg_file_aec_config(&aec_param);
  aec_param.output_way = 0u;
  aec_param.wideband = 1u;
  aec_param.dac_ref_sr = H2_AUDIO_SAMPLE_RATE;
  aec_param.hw_delay_offset = 30u;
  request.enc.aec_enable = aec_param.EnableBit != 0u ? 1u : 0u;
  request.enc.aec_attr = request.enc.aec_enable ? &aec_param : NULL;
  if (request.enc.aec_enable) {
    char message[160];
    int length = snprintf(
        message, sizeof(message),
        "H2_JIELI_AUDIO_AEC mode=dac-software-ref bits=0x%02x rate=%u delay=%u\r\n",
        (unsigned)aec_param.EnableBit, (unsigned)aec_param.dac_ref_sr,
        (unsigned)aec_param.hw_delay_offset);
    if (length > 0) {
      size_t bytes = (size_t)length < sizeof(message)
                         ? (size_t)length
                         : sizeof(message) - 1u;
      (void)h2_jieli_ac791n_devkit_console_write(message, bytes, 100u);
    }
  } else {
    audio_stage("H2_JIELI_AUDIO_AEC mode=disabled\r\n");
  }
#endif
  if (server_request(audio_state.mic_server, AUDIO_REQ_ENC, &request) != 0) {
    server_close(audio_state.mic_server);
    audio_state.mic_server = NULL;
    return H2_AUDIO_ERR_IO;
  }
  audio_state.mic_started = 1;
  return H2_AUDIO_OK;
}

static int audio_stop_mic(void *user) {
  (void)user;
  if (!audio_state.mic_started) return H2_AUDIO_OK;
  union audio_req request;
  memset(&request, 0, sizeof(request));
  request.enc.cmd = AUDIO_ENC_CLOSE;
  int result = server_request(
      audio_state.mic_server, AUDIO_REQ_ENC, &request);
  server_close(audio_state.mic_server);
  audio_state.mic_server = NULL;
  audio_state.mic_started = 0;
  os_sem_post(&audio_state.mic_ready);
  return result == 0 ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}

static int audio_start_speaker(void *user) {
  (void)user;
  audio_state.speaker_started = 1;
  return H2_AUDIO_OK;
}

static int audio_stop_speaker(void *user);

static int audio_mic_read(
    void *user, h2_audio_frame_t *frame, uint32_t wait_ms) {
  (void)user;
  if (!audio_state.mic_started) return H2_AUDIO_ERR_INVALID_STATE;
  if (frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      frame->channels != 1u || frame->capacity < sizeof(int16_t)) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  uint32_t available = cbuf_get_data_size(&audio_state.mic_pcm);
  if (available == 0u) {
    if (wait_ms == 0u ||
        os_sem_pend(&audio_state.mic_ready, timeout_ticks(wait_ms)) !=
            OS_NO_ERR) {
      return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    available = cbuf_get_data_size(&audio_state.mic_pcm);
  }
  /* Deliver one 20 ms frame per read, like the ESP and BK PAL queues.  Reading
   * every accumulated byte turns temporary scheduling jitter into audible
   * monitor latency. */
  if (available > H2_AUDIO_FRAME_BYTES) available = H2_AUDIO_FRAME_BYTES;
  if (available > frame->capacity) available = (uint32_t)frame->capacity;
  available &= ~1u;
  frame->bytes = cbuf_read(&audio_state.mic_pcm, frame->data, available);
  frame->sample_rate_hz = H2_AUDIO_SAMPLE_RATE;
  frame->samples_per_channel = (uint16_t)(frame->bytes / sizeof(int16_t));
  frame->channels = 1u;
  frame->sample_format = H2_AUDIO_SAMPLE_S16LE;
  return frame->bytes != 0u ? H2_AUDIO_OK : H2_AUDIO_ERR_WOULD_BLOCK;
}

static int track_write(
    h2_pal_audio_track_t *pal_track, const h2_audio_frame_t *frame,
    uint32_t wait_ms) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  if (!track->active || track->closed) return H2_PAL_ERR_CLOSED;
  if (frame->sample_rate_hz != H2_AUDIO_SAMPLE_RATE ||
      frame->channels != 1u ||
      frame->sample_format != H2_AUDIO_SAMPLE_S16LE) {
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  (void)wait_ms;
  if (frame->bytes > UINT32_MAX) return H2_AUDIO_ERR_INVALID_ARG;
  const void *write_data = frame->data;
  int32_t input_peak = 0;
  int32_t output_peak = 0;
  const int is_mic_track =
      track->name != NULL && strcmp(track->name, "audio-system-mic") == 0;
  if (is_mic_track &&
      frame->bytes <= sizeof(track->mic_monitor_frame) &&
      (frame->bytes % sizeof(int16_t)) == 0u) {
    const int16_t *input = (const int16_t *)frame->data;
    size_t count = frame->bytes / sizeof(*input);
    for (size_t index = 0u; index < count; ++index) {
      int32_t sample = input[index];
      int32_t magnitude = sample < 0 ? -sample : sample;
      if (magnitude > input_peak) input_peak = magnitude;
      sample *= H2_AUDIO_MIC_MONITOR_GAIN;
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      track->mic_monitor_frame[index] = (int16_t)sample;
      magnitude = sample < 0 ? -sample : sample;
      if (magnitude > output_peak) output_peak = magnitude;
    }
    write_data = track->mic_monitor_frame;
  }
  int written = audio_pcm_play_data_write(
      track->speaker_pcm, (void *)write_data, (uint32_t)frame->bytes);
  if (is_mic_track && written == (int)frame->bytes) {
    const uint32_t now_ms = timer_get_ms();
    if (track->rate_started_ms == 0u) {
      track->rate_started_ms = now_ms;
      track->rate_produced_bytes = audio_state.mic_produced_bytes;
      track->rate_produced_callbacks = audio_state.mic_produced_callbacks;
    }
    track->rate_written_bytes += (uint32_t)frame->bytes;
    const uint32_t elapsed_ms = now_ms - track->rate_started_ms;
    if (elapsed_ms >= 2000u) {
      const uint32_t produced_bytes = audio_state.mic_produced_bytes;
      const uint32_t produced_callbacks = audio_state.mic_produced_callbacks;
      const uint32_t produced_delta =
          produced_bytes - track->rate_produced_bytes;
      const uint32_t callback_delta =
          produced_callbacks - track->rate_produced_callbacks;
      char message[192];
      int length = snprintf(
          message, sizeof(message),
          "H2_JIELI_AUDIO_RATE elapsed_ms=%u produced_bps=%u callbacks=%u written_bps=%u input_peak=%ld output_peak=%ld\r\n",
          (unsigned)elapsed_ms,
          (unsigned)(((uint64_t)produced_delta * 1000u) / elapsed_ms),
          (unsigned)callback_delta,
          (unsigned)(((uint64_t)track->rate_written_bytes * 1000u) /
                     elapsed_ms),
          (long)input_peak, (long)output_peak);
      if (length > 0) {
        size_t bytes = (size_t)length < sizeof(message) ?
            (size_t)length : sizeof(message) - 1u;
        (void)h2_jieli_ac791n_devkit_console_write(message, bytes, 100u);
      }
      track->rate_started_ms = now_ms;
      track->rate_written_bytes = 0u;
      track->rate_produced_bytes = produced_bytes;
      track->rate_produced_callbacks = produced_callbacks;
    }
  }
  if (is_mic_track && (track->completed_writes & 63u) == 0u) {
    char message[160];
    int length = snprintf(
        message, sizeof(message),
        "H2_JIELI_AUDIO_TRACK name=mic bytes=%u written=%d input_peak=%ld output_peak=%ld\r\n",
        (unsigned)frame->bytes, written, (long)input_peak,
        (long)output_peak);
    if (length > 0) {
      size_t bytes = (size_t)length < sizeof(message) ?
          (size_t)length : sizeof(message) - 1u;
      (void)h2_jieli_ac791n_devkit_console_write(message, bytes, 100u);
    }
  }
  if (written < 0) return H2_AUDIO_ERR_IO;
  if ((size_t)written != frame->bytes) return H2_AUDIO_ERR_WOULD_BLOCK;
  ++track->completed_writes;
  return track->closed ? H2_PAL_ERR_CLOSED : H2_AUDIO_OK;
}

static int track_close(h2_pal_audio_track_t *pal_track) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  if (!track->active) return H2_AUDIO_OK;
  track->closed = 1;
  if (track->speaker_pcm != NULL) {
    (void)audio_pcm_play_stop(track->speaker_pcm);
    track->speaker_pcm = NULL;
  }
  track->active = 0;
  return H2_AUDIO_OK;
}

static int track_get_volume(
    h2_pal_audio_track_t *pal_track, uint32_t *out_factor_milli) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  *out_factor_milli = track->volume_factor_milli;
  return H2_AUDIO_OK;
}

static int track_set_volume(
    h2_pal_audio_track_t *pal_track, uint32_t factor_milli) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  if (factor_milli > 1000u) return H2_AUDIO_ERR_INVALID_ARG;
  track->volume_factor_milli = factor_milli;
  if (track->active && track->speaker_pcm != NULL) {
    if (audio_pcm_play_set_volume(
            track->speaker_pcm,
            (uint8_t)(audio_state.speaker_volume_percent * factor_milli /
                      1000u)) != 0) {
      return H2_AUDIO_ERR_IO;
    }
  }
  return H2_AUDIO_OK;
}

static int track_drain(
    h2_pal_audio_track_t *pal_track, uint32_t wait_ms) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  (void)wait_ms;
  return track->closed ? H2_PAL_ERR_CLOSED : H2_AUDIO_OK;
}

static int audio_create_track(
    void *user, const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
  (void)user;
  if (!audio_state.speaker_started) return H2_AUDIO_ERR_INVALID_STATE;
  if (config->format.sample_rate_hz != H2_AUDIO_SAMPLE_RATE ||
      config->format.channels != 1u ||
      config->format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      config->volume_factor_milli > 1000u) {
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  jieli_audio_track_t *track = NULL;
  for (size_t index = 0u; index < H2_AUDIO_MAX_TRACKS; ++index) {
    if (!audio_state.tracks[index].active) {
      track = &audio_state.tracks[index];
      break;
    }
  }
  if (track == NULL) return H2_PAL_ERR_BUSY;
  memset(track, 0, sizeof(*track));
  track->active = 1;
  track->name = config->name;
  track->closed = 0;
  track->completed_writes = 0u;
  track->volume_factor_milli = config->volume_factor_milli;
  track->pal = (h2_pal_audio_track_t){
      .user = track,
      .audio = h2_jieli_ac791n_devkit_audio_api(),
      .write = track_write,
      .close = track_close,
      .get_volume_factor = track_get_volume,
      .set_volume_factor = track_set_volume,
      .drain = track_drain,
  };
  const size_t queue_frames = config->buffer_frames != 0u
                                  ? config->buffer_frames
                                  : H2_AUDIO_TRACK_QUEUE_FRAMES;
  const size_t wanted_cache_bytes = H2_AUDIO_FRAME_BYTES * queue_frames;
  /* audio_pcm_play_open() internally allocates frame_size * 4 bytes.  Pass a
   * quarter of the requested PAL capacity so buffer_frames retains its public
   * meaning instead of every track silently getting roughly 500 ms of audio. */
  const uint32_t pcm_frame_size =
      (uint32_t)((wanted_cache_bytes + 3u) / 4u);
  audio_stage("H2_JIELI_AUDIO stage=pcm-open-before\r\n");
  track->speaker_pcm = audio_pcm_play_open_no_sync(
      H2_AUDIO_SAMPLE_RATE, pcm_frame_size, 0u, 1u,
      (uint8_t)(audio_state.speaker_volume_percent *
                config->volume_factor_milli / 1000u),
      0u);
  if (track->speaker_pcm == NULL) {
    track->active = 0;
    track->closed = 1;
    return H2_AUDIO_ERR_UNAVAILABLE;
  }
  audio_stage("H2_JIELI_AUDIO stage=pcm-open-after\r\n");
  audio_stage("H2_JIELI_AUDIO stage=pcm-start-before\r\n");
  if (audio_pcm_play_start(track->speaker_pcm) != 0) {
    (void)audio_pcm_play_stop(track->speaker_pcm);
    track->speaker_pcm = NULL;
    track->active = 0;
    track->closed = 1;
    return H2_AUDIO_ERR_IO;
  }
  if (audio_pcm_play_set_block(track->speaker_pcm, 1u) != 0) {
    (void)audio_pcm_play_stop(track->speaker_pcm);
    track->speaker_pcm = NULL;
    track->active = 0;
    track->closed = 1;
    return H2_AUDIO_ERR_IO;
  }
  audio_stage("H2_JIELI_AUDIO stage=pcm-start-after\r\n");
  *out_track = &track->pal;
  return H2_AUDIO_OK;
}

static int audio_stop_speaker(void *user) {
  (void)user;
  audio_state.speaker_started = 0;
  int result = H2_AUDIO_OK;
  for (size_t index = 0u; index < H2_AUDIO_MAX_TRACKS; ++index) {
    int close_result = track_close(&audio_state.tracks[index].pal);
    if (result == H2_AUDIO_OK) result = close_result;
  }
  return result;
}

static int audio_get_volume(void *user, uint32_t *out_percent) {
  (void)user;
  *out_percent = audio_state.speaker_volume_percent;
  return H2_AUDIO_OK;
}

static int audio_set_volume(void *user, uint32_t percent) {
  (void)user;
  if (percent > 100u) return H2_AUDIO_ERR_INVALID_ARG;
  audio_state.speaker_volume_percent = percent;
  for (size_t index = 0u; index < H2_AUDIO_MAX_TRACKS; ++index) {
    jieli_audio_track_t *track = &audio_state.tracks[index];
    if (track->active && track->speaker_pcm != NULL) {
      if (audio_pcm_play_set_volume(
              track->speaker_pcm,
              (uint8_t)(percent * track->volume_factor_milli / 1000u)) != 0) {
        return H2_AUDIO_ERR_IO;
      }
    }
  }
  return H2_AUDIO_OK;
}

const h2_pal_audio_api_t *h2_jieli_ac791n_devkit_audio_api(void) {
  static const h2_pal_audio_vtable_t vtable = {
      .get_info = audio_get_info,
      .start_mic = audio_start_mic,
      .stop_mic = audio_stop_mic,
      .start_speaker = audio_start_speaker,
      .stop_speaker = audio_stop_speaker,
      .mic_read = audio_mic_read,
      .create_track = audio_create_track,
      .get_speaker_volume_percent = audio_get_volume,
      .set_speaker_volume_percent = audio_set_volume,
  };
  static const h2_pal_audio_api_t api = {
      .user = &audio_state,
      .vtable = &vtable,
  };
  return &api;
}

#else

#include "h2_jieli_ac791n_devkit.h"

extern const h2_pal_audio_api_t *h2_pal_unsupported_audio_api(void);

const h2_pal_audio_api_t *h2_jieli_ac791n_devkit_audio_api(void) {
  return h2_pal_unsupported_audio_api();
}

#endif
