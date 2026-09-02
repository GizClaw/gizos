#include "h2_bloomspeaker_audio.h"

#include "h2_bloomspeaker_task_names.h"

#include "h2/pal/hal/h2_pal_audio.h"

#include "opus.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define H2_BLOOMSPEAKER_AUDIO_RATE 16000u
#define H2_BLOOMSPEAKER_AUDIO_FRAME_SAMPLES 320u
#define H2_BLOOMSPEAKER_AUDIO_PACKET_MAX 200u
#define H2_BLOOMSPEAKER_AUDIO_HEADER_SIZE 5u
#define H2_BLOOMSPEAKER_AUDIO_TYPE 0xa1u
#define H2_BLOOMSPEAKER_AUDIO_TX_TIMEOUT_MS 0u
#define H2_BLOOMSPEAKER_AUDIO_RX_TIMEOUT_MS 50u
#define H2_BLOOMSPEAKER_AUDIO_JITTER_FRAMES 3u
#define H2_BLOOMSPEAKER_AUDIO_DECODE_MAX_SAMPLES 960u
#define H2_BLOOMSPEAKER_AUDIO_TASK_STACK_SIZE (32u * 1024u)

typedef struct h2_bloomspeaker_encoded_frame {
  uint16_t sequence;
  uint16_t size;
  uint8_t payload[H2_BLOOMSPEAKER_AUDIO_PACKET_MAX];
} h2_bloomspeaker_encoded_frame_t;

struct h2_bloomspeaker_audio {
  h2_runtime_t *runtime;
  h2_bloomspeaker_controller_t *controller;
  h2_audio_info_t info;
  h2_pal_mutex_t *stream_mutex;
  h2_pal_task_t *capture_task;
  _Atomic int stop;
  _Atomic bool stream_active;
  h2_bleikcp_t *stream;
  OpusEncoder *encoder;
  int16_t *mic_buffer;
  size_t mic_buffer_bytes;
  int16_t *accumulator;
  size_t accumulator_capacity;
  size_t accumulator_count;
  uint16_t tx_sequence;
};

static void write_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8u);
}

static uint16_t read_u16(const uint8_t *data) {
  return (uint16_t)((uint16_t)data[0] | (uint16_t)data[1] << 8u);
}

static void pcm_level(const int16_t *pcm, size_t samples, float *out_level,
                      float *out_peak) {
  double squares = 0.0;
  int32_t peak = 0;
  for (size_t index = 0u; index < samples; ++index) {
    int32_t value = pcm[index];
    int32_t magnitude = value < 0 ? -value : value;
    if (magnitude > peak) {
      peak = magnitude;
    }
    squares += (double)value * (double)value;
  }
  *out_level = samples == 0u
                   ? 0.0f
                   : (float)(sqrt(squares / (double)samples) / 32768.0);
  *out_peak = (float)peak / 32768.0f;
}

static int send_pcm(h2_bloomspeaker_audio_t *audio, const int16_t *pcm) {
  uint8_t packet[H2_BLOOMSPEAKER_AUDIO_HEADER_SIZE +
                 H2_BLOOMSPEAKER_AUDIO_PACKET_MAX];
  int encoded = opus_encode(audio->encoder, pcm,
                            H2_BLOOMSPEAKER_AUDIO_FRAME_SAMPLES,
                            packet + H2_BLOOMSPEAKER_AUDIO_HEADER_SIZE,
                            H2_BLOOMSPEAKER_AUDIO_PACKET_MAX);
  if (encoded < 0) {
    return H2_PAL_ERR_IO;
  }
  packet[0] = H2_BLOOMSPEAKER_AUDIO_TYPE;
  write_u16(packet + 1u, audio->tx_sequence++);
  write_u16(packet + 3u, (uint16_t)encoded);
  if (h2_pal_mutex_lock(audio->runtime->sync, audio->stream_mutex) !=
      H2_PAL_OK) {
    return H2_PAL_ERR_BUSY;
  }
  int result = H2_PAL_OK;
  if (audio->stream != NULL) {
    result = h2_bleikcp_write(
        audio->stream, packet,
        H2_BLOOMSPEAKER_AUDIO_HEADER_SIZE + (size_t)encoded,
        H2_BLOOMSPEAKER_AUDIO_TX_TIMEOUT_MS);
    if (result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK) {
      /* Real-time audio drops a late capture frame instead of growing delay. */
      result = H2_PAL_OK;
    }
  }
  (void)h2_pal_mutex_unlock(audio->runtime->sync, audio->stream_mutex);
  return result;
}

static int process_capture(h2_bloomspeaker_audio_t *audio,
                           const h2_audio_frame_t *frame) {
  size_t samples = frame->bytes / sizeof(int16_t);
  float level = 0.0f;
  float peak = 0.0f;
  pcm_level(frame->data, samples, &level, &peak);
  h2_bloomspeaker_controller_set_local_levels(audio->controller, level, peak);
  if (audio->accumulator_count + samples > audio->accumulator_capacity) {
    audio->accumulator_count = 0u;
    return H2_PAL_ERR_NO_SPACE;
  }
  memcpy(audio->accumulator + audio->accumulator_count, frame->data,
         samples * sizeof(int16_t));
  audio->accumulator_count += samples;
  while (audio->accumulator_count >= H2_BLOOMSPEAKER_AUDIO_FRAME_SAMPLES) {
    int result = H2_PAL_OK;
    if (atomic_load_explicit(&audio->stream_active, memory_order_acquire)) {
      result = send_pcm(audio, audio->accumulator);
    }
    if (result != H2_PAL_OK && result != H2_PAL_ERR_CLOSED) {
      return result;
    }
    audio->accumulator_count -= H2_BLOOMSPEAKER_AUDIO_FRAME_SAMPLES;
    memmove(audio->accumulator,
            audio->accumulator + H2_BLOOMSPEAKER_AUDIO_FRAME_SAMPLES,
            audio->accumulator_count * sizeof(int16_t));
  }
  return H2_PAL_OK;
}

static void capture_task(void *context) {
  h2_bloomspeaker_audio_t *audio = context;
  while (!atomic_load_explicit(&audio->stop, memory_order_acquire)) {
    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        audio->mic_buffer, audio->mic_buffer_bytes, audio->info.mic_format);
    int result = h2_pal_audio_mic_read(audio->runtime->audio, &frame, 100u);
    if (result == H2_AUDIO_OK && frame.bytes != 0u) {
      (void)process_capture(audio, &frame);
    } else if (result != H2_AUDIO_ERR_WOULD_BLOCK &&
               result != H2_PAL_ERR_TIMEOUT) {
      (void)h2_pal_time_sleep_ms(audio->runtime->time, 20u);
    }
  }
}

static int set_stream(h2_bloomspeaker_audio_t *audio,
                      h2_bleikcp_t *stream) {
  int result = h2_pal_mutex_lock(audio->runtime->sync, audio->stream_mutex);
  if (result == H2_PAL_OK) {
    audio->stream = stream;
    audio->tx_sequence = 0u;
    atomic_store_explicit(&audio->stream_active, stream != NULL,
                          memory_order_release);
    result = h2_pal_mutex_unlock(audio->runtime->sync, audio->stream_mutex);
  }
  return result;
}

static int read_exact(h2_bleikcp_t *stream, uint8_t *out, size_t size,
                      h2_bloomspeaker_audio_session_active_fn active,
                      void *active_user) {
  size_t offset = 0u;
  while (offset < size && active(active_user)) {
    size_t received = 0u;
    int result = h2_bleikcp_read(stream, out + offset, size - offset,
                                 &received,
                                 H2_BLOOMSPEAKER_AUDIO_RX_TIMEOUT_MS);
    if (result == H2_PAL_OK) {
      offset += received;
    } else if (result != H2_PAL_ERR_TIMEOUT &&
               result != H2_PAL_ERR_WOULD_BLOCK) {
      return result;
    }
  }
  return offset == size ? H2_PAL_OK : H2_PAL_ERR_CLOSED;
}

static int read_encoded_frame(
    h2_bleikcp_t *stream, h2_bloomspeaker_encoded_frame_t *out,
    h2_bloomspeaker_audio_session_active_fn active, void *active_user) {
  uint8_t header[H2_BLOOMSPEAKER_AUDIO_HEADER_SIZE];
  int result = read_exact(stream, header, sizeof(header), active, active_user);
  if (result != H2_PAL_OK) {
    return result;
  }
  uint16_t size = read_u16(header + 3u);
  if (header[0] != H2_BLOOMSPEAKER_AUDIO_TYPE || size == 0u ||
      size > sizeof(out->payload)) {
    return H2_PAL_ERR_FORMAT;
  }
  out->sequence = read_u16(header + 1u);
  out->size = size;
  return read_exact(stream, out->payload, size, active, active_user);
}

static int write_decoded(h2_bloomspeaker_audio_t *audio,
                         h2_pal_audio_track_t *track, OpusDecoder *decoder,
                         const uint8_t *packet, size_t packet_size,
                         int16_t *playback_buffer,
                         size_t playback_capacity,
                         size_t *playback_count) {
  int16_t pcm[H2_BLOOMSPEAKER_AUDIO_DECODE_MAX_SAMPLES];
  int decoded = opus_decode(decoder, packet, (opus_int32)packet_size, pcm,
                            H2_BLOOMSPEAKER_AUDIO_DECODE_MAX_SAMPLES, 0);
  if (decoded <= 0) {
    return H2_PAL_ERR_IO;
  }
  float level = 0.0f;
  float peak = 0.0f;
  pcm_level(pcm, (size_t)decoded, &level, &peak);
  h2_bloomspeaker_controller_set_remote_levels(audio->controller, level, peak);
  size_t offset = 0u;
  while (offset < (size_t)decoded) {
    size_t copy_count = playback_capacity - *playback_count;
    size_t remaining = (size_t)decoded - offset;
    if (copy_count > remaining) {
      copy_count = remaining;
    }
    memcpy(playback_buffer + *playback_count, pcm + offset,
           copy_count * sizeof(int16_t));
    *playback_count += copy_count;
    offset += copy_count;
    if (*playback_count == playback_capacity) {
      h2_audio_frame_t frame = h2_audio_frame_for_buffer(
          playback_buffer, playback_capacity * sizeof(int16_t),
          audio->info.playback_format);
      frame.bytes = playback_capacity * sizeof(int16_t);
      int result = h2_pal_audio_track_write(track, &frame, 100u);
      if (result != H2_PAL_OK) {
        return result;
      }
      *playback_count = 0u;
    }
  }
  return H2_PAL_OK;
}

int h2_bloomspeaker_audio_run_session(
    h2_bloomspeaker_audio_t *audio, h2_bleikcp_t *stream,
    h2_bloomspeaker_audio_session_active_fn active, void *active_user) {
  if (audio == NULL || stream == NULL || active == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int decoder_size = opus_decoder_get_size(1);
  OpusDecoder *decoder = decoder_size > 0
                             ? h2_pal_mem_alloc(audio->runtime->mem,
                                                (size_t)decoder_size)
                             : NULL;
  size_t playback_capacity =
      audio->info.playback_format.frame_samples_per_channel;
  int16_t *playback_buffer = playback_capacity > 0u
                                 ? h2_pal_mem_alloc(
                                       audio->runtime->mem,
                                       playback_capacity * sizeof(int16_t))
                                 : NULL;
  if (decoder == NULL || playback_buffer == NULL) {
    h2_pal_mem_free(audio->runtime->mem, playback_buffer);
    h2_pal_mem_free(audio->runtime->mem, decoder);
    return H2_PAL_ERR_NO_MEMORY;
  }
  int result = opus_decoder_init(decoder, H2_BLOOMSPEAKER_AUDIO_RATE, 1) ==
                       OPUS_OK
                   ? H2_PAL_OK
                   : H2_PAL_ERR_IO;
  h2_pal_audio_track_t *track = NULL;
  bool speaker_started = false;
  if (result == H2_PAL_OK) {
    result = h2_pal_audio_start_speaker(audio->runtime->audio);
    speaker_started = result == H2_PAL_OK;
  }
  const h2_audio_track_config_t track_config = {
      .name = "lua-bloomspeaker/remote",
      .format = audio->info.playback_format,
      .volume_factor_milli = 1000u,
      .buffer_frames = 6u,
  };
  if (result == H2_PAL_OK) {
    result = h2_pal_audio_create_track(audio->runtime->audio, &track_config,
                                       &track);
  }
  if (result == H2_PAL_OK) {
    result = set_stream(audio, stream);
  }
  h2_bloomspeaker_encoded_frame_t
      jitter[H2_BLOOMSPEAKER_AUDIO_JITTER_FRAMES];
  size_t buffered = 0u;
  uint16_t expected_sequence = 0u;
  bool sequence_started = false;
  size_t playback_count = 0u;
  while (result == H2_PAL_OK && active(active_user)) {
    while (buffered < H2_BLOOMSPEAKER_AUDIO_JITTER_FRAMES &&
           active(active_user)) {
      result = read_encoded_frame(stream, &jitter[buffered], active,
                                  active_user);
      if (result != H2_PAL_OK) {
        break;
      }
      buffered++;
    }
    if (result != H2_PAL_OK || buffered == 0u) {
      break;
    }
    if (!sequence_started) {
      expected_sequence = jitter[0].sequence;
      sequence_started = true;
    }
    uint16_t missing = (uint16_t)(jitter[0].sequence - expected_sequence);
    if (missing < UINT16_C(0x8000)) {
      if (missing > 2u) {
        missing = 2u;
      }
      for (uint16_t index = 0u; index < missing && result == H2_PAL_OK;
           ++index) {
        result = write_decoded(audio, track, decoder, NULL, 0u,
                               playback_buffer, playback_capacity,
                               &playback_count);
      }
    }
    if (result == H2_PAL_OK) {
      result = write_decoded(audio, track, decoder, jitter[0].payload,
                             jitter[0].size, playback_buffer,
                             playback_capacity, &playback_count);
    }
    expected_sequence = (uint16_t)(jitter[0].sequence + 1u);
    if (buffered > 1u) {
      memmove(jitter, jitter + 1u,
              (buffered - 1u) * sizeof(jitter[0]));
    }
    buffered--;
  }
  (void)set_stream(audio, NULL);
  h2_bloomspeaker_controller_set_remote_levels(audio->controller, 0.0f,
                                                0.0f);
  if (track != NULL) {
    (void)h2_pal_audio_track_close(track);
  }
  if (speaker_started) {
    (void)h2_pal_audio_stop_speaker(audio->runtime->audio);
  }
  h2_pal_mem_free(audio->runtime->mem, playback_buffer);
  h2_pal_mem_free(audio->runtime->mem, decoder);
  return result == H2_PAL_ERR_CLOSED && !active(active_user) ? H2_PAL_OK
                                                              : result;
}

int h2_bloomspeaker_audio_start(h2_runtime_t *runtime,
                                h2_bloomspeaker_controller_t *controller,
                                h2_bloomspeaker_audio_t **out_audio) {
  if (runtime == NULL || controller == NULL || out_audio == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_audio = NULL;
  h2_audio_info_t info;
  int result = h2_pal_audio_get_info(runtime->audio, &info);
  if (result != H2_AUDIO_OK || !info.available || !info.mic_supported ||
      !info.playback_supported) {
    return H2_PAL_OK;
  }
  if (info.mic_format.sample_rate_hz != H2_BLOOMSPEAKER_AUDIO_RATE ||
      info.mic_format.channels != 1u ||
      info.mic_format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      info.playback_format.sample_rate_hz != H2_BLOOMSPEAKER_AUDIO_RATE ||
      info.playback_format.channels != 1u ||
      info.playback_format.sample_format != H2_AUDIO_SAMPLE_S16LE) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_bloomspeaker_audio_t *audio =
      h2_pal_mem_alloc(runtime->mem, sizeof(*audio));
  if (audio == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(audio, 0, sizeof(*audio));
  audio->runtime = runtime;
  audio->controller = controller;
  audio->info = info;
  atomic_init(&audio->stop, 0);
  atomic_init(&audio->stream_active, false);
  audio->mic_buffer_bytes =
      (size_t)info.mic_format.frame_samples_per_channel * sizeof(int16_t);
  audio->accumulator_capacity =
      (size_t)info.mic_format.frame_samples_per_channel +
      H2_BLOOMSPEAKER_AUDIO_FRAME_SAMPLES * 2u;
  int encoder_size = opus_encoder_get_size(1);
  audio->mic_buffer = h2_pal_mem_alloc(runtime->mem, audio->mic_buffer_bytes);
  audio->accumulator = h2_pal_mem_alloc(
      runtime->mem, audio->accumulator_capacity * sizeof(int16_t));
  audio->encoder = encoder_size > 0
                       ? h2_pal_mem_alloc(runtime->mem, (size_t)encoder_size)
                       : NULL;
  if (audio->mic_buffer == NULL || audio->accumulator == NULL ||
      audio->encoder == NULL) {
    result = H2_PAL_ERR_NO_MEMORY;
  }
  if (result == H2_PAL_OK &&
      opus_encoder_init(audio->encoder, H2_BLOOMSPEAKER_AUDIO_RATE, 1,
                        OPUS_APPLICATION_VOIP) != OPUS_OK) {
    result = H2_PAL_ERR_IO;
  }
  if (result == H2_PAL_OK) {
    (void)opus_encoder_ctl(audio->encoder, OPUS_SET_BITRATE(18000));
    (void)opus_encoder_ctl(audio->encoder, OPUS_SET_COMPLEXITY(3));
    (void)opus_encoder_ctl(audio->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    (void)opus_encoder_ctl(audio->encoder, OPUS_SET_INBAND_FEC(1));
    (void)opus_encoder_ctl(audio->encoder, OPUS_SET_PACKET_LOSS_PERC(8));
    const h2_pal_mutex_config_t mutex_config = {
        .name = "lua-bloomspeaker/audio-stream",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    result = h2_pal_mutex_create(runtime->sync, &mutex_config,
                                 &audio->stream_mutex);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_audio_start_mic(runtime->audio);
  }
  bool mic_started = result == H2_PAL_OK;
  const h2_pal_task_options_t task_options = {
      .name = h2_bloomspeaker_audio_task_name,
      .min_stack_size = H2_BLOOMSPEAKER_AUDIO_TASK_STACK_SIZE,
  };
  if (result == H2_PAL_OK) {
    result = h2_pal_task_start(runtime->task, &task_options, capture_task,
                               audio, &audio->capture_task);
  }
  if (result != H2_PAL_OK) {
    if (mic_started) {
      (void)h2_pal_audio_stop_mic(runtime->audio);
    }
    if (audio->stream_mutex != NULL) {
      (void)h2_pal_mutex_destroy(runtime->sync, audio->stream_mutex);
    }
    h2_pal_mem_free(runtime->mem, audio->encoder);
    h2_pal_mem_free(runtime->mem, audio->accumulator);
    h2_pal_mem_free(runtime->mem, audio->mic_buffer);
    h2_pal_mem_free(runtime->mem, audio);
    return result;
  }
  h2_bloomspeaker_controller_set_native_audio(controller, true);
  *out_audio = audio;
  return H2_PAL_OK;
}

int h2_bloomspeaker_audio_stop(h2_bloomspeaker_audio_t *audio) {
  if (audio == NULL) {
    return H2_PAL_OK;
  }
  atomic_store_explicit(&audio->stop, 1, memory_order_release);
  int result = h2_pal_task_join(audio->runtime->task, audio->capture_task);
  if (result != H2_PAL_OK) {
    return result;
  }
  (void)h2_pal_audio_stop_mic(audio->runtime->audio);
  h2_bloomspeaker_controller_set_native_audio(audio->controller, false);
  (void)h2_pal_mutex_destroy(audio->runtime->sync, audio->stream_mutex);
  h2_pal_mem_free(audio->runtime->mem, audio->encoder);
  h2_pal_mem_free(audio->runtime->mem, audio->accumulator);
  h2_pal_mem_free(audio->runtime->mem, audio->mic_buffer);
  h2_pal_mem_free(audio->runtime->mem, audio);
  return H2_PAL_OK;
}
