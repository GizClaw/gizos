#include "h2_android_platform.h"
#include "h2_android_audio_internal.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct h2_android_audio_track {
  h2_pal_audio_track_t base;
  struct h2_android_platform *host;
  AAudioStream *stream;
  h2_audio_pcm_format_t format;
  uint32_t volume_factor_milli;
  int16_t *pending_samples;
  uint32_t pending_frame_count;
  uint32_t pending_frame_offset;
  h2_pal_mem_api_t allocator;
} h2_android_audio_track_t;

static void *android_track_alloc(h2_android_audio_track_t *track, size_t bytes) {
  return track->allocator.vtable != NULL
      ? h2_pal_mem_alloc(&track->allocator, bytes) : malloc(bytes);
}

static void android_track_free(h2_android_audio_track_t *track, void *ptr) {
  if (track->allocator.vtable != NULL)
    h2_pal_mem_free(&track->allocator, ptr);
  else free(ptr);
}

struct h2_android_platform {
  pthread_mutex_t mutex;
  JavaVM *vm;
  jobject view;
  jmethodID request_frame;
  int32_t width;
  int32_t height;
  uint32_t *rgba;
  int32_t pointer_x;
  int32_t pointer_y;
  int pointer_pressed;
  h2_pal_audio_api_t audio;
  h2_android_audio_track_t *audio_track;
  AAudioStream *mic_stream;
  uint32_t mic_gain_percent;
  uint32_t speaker_volume_percent;
  int speaker_started;
  h2_pal_display_api_t display;
  int opened;
  uint32_t brightness_percent;
};

static int android_audio_get_info(void *user, h2_audio_info_t *out_info) {
  if (user == NULL || out_info == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  *out_info = (h2_audio_info_t){
      .available = 1,
      .mic_supported = 1,
      .mic_format = {16000u, 320u, 1u, H2_AUDIO_SAMPLE_S16LE},
      .mic_queue_frames = 1u,
      .playback_supported = 1,
      .playback_format =
          {
              .sample_rate_hz = 16000u,
              .frame_samples_per_channel = 512u,
              .channels = 1u,
              .sample_format = H2_AUDIO_SAMPLE_S16LE,
          },
      .track_queue_frames = 16u,
      .max_tracks = 1u,
  };
  return H2_PAL_OK;
}

static int android_audio_start_mic(void *user) {
  h2_android_platform_t *host = user;
  if (host == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&host->mutex);
  if (host->mic_stream != NULL) {
    pthread_mutex_unlock(&host->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  AAudioStreamBuilder *builder = NULL;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK || builder == NULL) {
    pthread_mutex_unlock(&host->mutex);
    return H2_AUDIO_ERR_IO;
  }
  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setSampleRate(builder, 16000);
  AAudioStreamBuilder_setChannelCount(builder, 1);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
  AAudioStream *stream = NULL;
  result = AAudioStreamBuilder_openStream(builder, &stream);
  (void)AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK || stream == NULL) {
    pthread_mutex_unlock(&host->mutex);
    return H2_AUDIO_ERR_UNAVAILABLE;
  }
  if (AAudioStream_getSampleRate(stream) != 16000 ||
      AAudioStream_getChannelCount(stream) != 1 ||
      AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_I16) {
    (void)AAudioStream_close(stream);
    pthread_mutex_unlock(&host->mutex);
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  result = AAudioStream_requestStart(stream);
  if (result != AAUDIO_OK) {
    (void)AAudioStream_close(stream);
    pthread_mutex_unlock(&host->mutex);
    return H2_AUDIO_ERR_UNAVAILABLE;
  }
  host->mic_stream = stream;
  pthread_mutex_unlock(&host->mutex);
  return H2_AUDIO_OK;
}

static int android_audio_stop_mic(void *user) {
  h2_android_platform_t *host = user;
  if (host == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&host->mutex);
  AAudioStream *stream = host->mic_stream;
  host->mic_stream = NULL;
  int rc = H2_AUDIO_OK;
  if (stream != NULL) {
    const aaudio_result_t stop = AAudioStream_requestStop(stream);
    const aaudio_result_t close = AAudioStream_close(stream);
    if (stop != AAUDIO_OK || close != AAUDIO_OK) rc = H2_AUDIO_ERR_IO;
  }
  pthread_mutex_unlock(&host->mutex);
  return rc;
}

static int android_audio_mic_read(void *user, h2_audio_frame_t *out_frame,
                                  uint32_t timeout_ms) {
  h2_android_platform_t *host = user;
  if (host == NULL || out_frame == NULL || out_frame->data == NULL ||
      out_frame->capacity < 320u * sizeof(int16_t) ||
      out_frame->sample_rate_hz != 16000u || out_frame->channels != 1u ||
      out_frame->sample_format != H2_AUDIO_SAMPLE_S16LE)
    return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&host->mutex);
  if (host->mic_stream == NULL) {
    pthread_mutex_unlock(&host->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  const int64_t timeout_ns = (int64_t)timeout_ms * 1000000;
  const aaudio_result_t read = AAudioStream_read(
      host->mic_stream, out_frame->data, 320, timeout_ns);
  if (read <= 0) {
    pthread_mutex_unlock(&host->mutex);
    return read == 0 ? H2_AUDIO_ERR_WOULD_BLOCK : H2_AUDIO_ERR_IO;
  }
  const uint32_t gain = host->mic_gain_percent;
  int16_t *samples = out_frame->data;
  for (aaudio_result_t i = 0; i < read; ++i) {
    const int32_t scaled = (int32_t)samples[i] * (int32_t)gain / 100;
    samples[i] = (int16_t)scaled;
  }
  out_frame->bytes = (size_t)read * sizeof(int16_t);
  out_frame->samples_per_channel = (uint16_t)read;
  pthread_mutex_unlock(&host->mutex);
  return H2_AUDIO_OK;
}

static int android_audio_start_speaker(void *user) {
  h2_android_platform_t *host = user;
  if (host == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  const int result = host->speaker_started ? H2_AUDIO_ERR_INVALID_STATE
                                            : H2_PAL_OK;
  if (result == H2_PAL_OK) {
    host->speaker_started = 1;
  }
  pthread_mutex_unlock(&host->mutex);
  return result;
}

static int android_audio_stop_speaker(void *user) {
  h2_android_platform_t *host = user;
  if (host == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  const int result = !host->speaker_started || host->audio_track != NULL
                         ? H2_AUDIO_ERR_INVALID_STATE
                         : H2_PAL_OK;
  if (result == H2_PAL_OK) {
    host->speaker_started = 0;
  }
  pthread_mutex_unlock(&host->mutex);
  return result;
}

static int android_audio_track_flush_pending(h2_android_audio_track_t *track,
                                             int64_t timeout_ns) {
  while (track->pending_frame_offset < track->pending_frame_count) {
    const aaudio_result_t result = AAudioStream_write(
        track->stream,
        track->pending_samples +
            (size_t)track->pending_frame_offset * track->format.channels,
        (int32_t)(track->pending_frame_count - track->pending_frame_offset),
        timeout_ns);
    if (result <= 0) {
      return result == 0
                 ? H2_AUDIO_ERR_WOULD_BLOCK
                 : h2_android_audio_map_aaudio_write_result(
                       result, AAUDIO_ERROR_TIMEOUT);
    }
    if ((uint32_t)result >
        track->pending_frame_count - track->pending_frame_offset) {
      return H2_AUDIO_ERR_IO;
    }
    track->pending_frame_offset += (uint32_t)result;
  }
  android_track_free(track, track->pending_samples);
  track->pending_samples = NULL;
  track->pending_frame_count = 0u;
  track->pending_frame_offset = 0u;
  return H2_PAL_OK;
}

static int android_audio_track_write(h2_pal_audio_track_t *base,
                                     const h2_audio_frame_t *frame,
                                     uint32_t timeout_ms) {
  h2_android_audio_track_t *track = (h2_android_audio_track_t *)base;
  if (track == NULL ||
      h2_android_audio_validate_playback_frame(&track->format, frame) !=
          H2_PAL_OK) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&track->host->mutex);
  const uint32_t factor = track->volume_factor_milli;
  const uint32_t percent = track->host->speaker_volume_percent;
  pthread_mutex_unlock(&track->host->mutex);

  const int64_t timeout_ns = (int64_t)timeout_ms * 1000000;
  const int pending_result =
      android_audio_track_flush_pending(track, timeout_ns);
  if (pending_result != H2_PAL_OK) {
    return pending_result;
  }

  int16_t *samples = android_track_alloc(track, frame->bytes);
  if (samples == NULL) {
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  const int16_t *source = frame->data;
  const size_t sample_count = frame->bytes / sizeof(int16_t);
  for (size_t i = 0u; i < sample_count; ++i) {
    int64_t value =
        (int64_t)source[i] * (int64_t)factor * (int64_t)percent / 100000;
    if (value > INT16_MAX) {
      value = INT16_MAX;
    } else if (value < INT16_MIN) {
      value = INT16_MIN;
    }
    samples[i] = (int16_t)value;
  }

  uint32_t written = 0u;
  while (written < frame->samples_per_channel) {
    const aaudio_result_t result = AAudioStream_write(
        track->stream,
        samples + (size_t)written * track->format.channels,
        (int32_t)(frame->samples_per_channel - written), timeout_ns);
    if (result <= 0) {
      if (h2_android_audio_should_retain_partial_write(
              written, frame->samples_per_channel, result)) {
        track->pending_samples = samples;
        track->pending_frame_count = frame->samples_per_channel;
        track->pending_frame_offset = written;
        return H2_PAL_OK;
      }
      android_track_free(track, samples);
      return result == 0
                 ? H2_AUDIO_ERR_WOULD_BLOCK
                 : h2_android_audio_map_aaudio_write_result(
                       result, AAUDIO_ERROR_TIMEOUT);
    }
    if ((uint32_t)result > frame->samples_per_channel - written) {
      android_track_free(track, samples);
      return H2_AUDIO_ERR_IO;
    }
    written += (uint32_t)result;
  }
  android_track_free(track, samples);
  return H2_PAL_OK;
}

static int android_audio_track_get_volume_factor(
    h2_pal_audio_track_t *base, uint32_t *out_factor_milli) {
  h2_android_audio_track_t *track = (h2_android_audio_track_t *)base;
  if (track == NULL || out_factor_milli == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&track->host->mutex);
  *out_factor_milli = track->volume_factor_milli;
  pthread_mutex_unlock(&track->host->mutex);
  return H2_PAL_OK;
}

static int android_audio_track_set_volume_factor(h2_pal_audio_track_t *base,
                                                 uint32_t factor_milli) {
  h2_android_audio_track_t *track = (h2_android_audio_track_t *)base;
  if (track == NULL || factor_milli > 1000u) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&track->host->mutex);
  track->volume_factor_milli = factor_milli;
  pthread_mutex_unlock(&track->host->mutex);
  return H2_PAL_OK;
}

static int android_audio_track_drain(h2_pal_audio_track_t *base,
                                     uint32_t timeout_ms) {
  h2_android_audio_track_t *track = (h2_android_audio_track_t *)base;
  if (track == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  const int64_t timeout_ns = (int64_t)timeout_ms * 1000000;
  const int pending_result =
      android_audio_track_flush_pending(track, timeout_ns);
  if (pending_result != H2_PAL_OK) {
    return pending_result;
  }
  uint32_t elapsed_ms = 0u;
  while (AAudioStream_getFramesRead(track->stream) <
         AAudioStream_getFramesWritten(track->stream)) {
    if (timeout_ms != UINT32_MAX && elapsed_ms >= timeout_ms) {
      return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    struct timespec delay = {.tv_nsec = 1000000l};
    (void)nanosleep(&delay, NULL);
    ++elapsed_ms;
  }
  return H2_PAL_OK;
}

static int android_audio_track_close(h2_pal_audio_track_t *base) {
  h2_android_audio_track_t *track = (h2_android_audio_track_t *)base;
  if (track == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  h2_android_platform_t *host = track->host;
  (void)AAudioStream_requestStop(track->stream);
  const aaudio_result_t close_result = AAudioStream_close(track->stream);
  android_track_free(track, track->pending_samples);
  pthread_mutex_lock(&host->mutex);
  if (host->audio_track == track) {
    host->audio_track = NULL;
  }
  pthread_mutex_unlock(&host->mutex);
  android_track_free(track, track);
  return h2_android_audio_map_aaudio_io_result(close_result);
}

static int android_audio_create_track(void *user,
                                      const h2_audio_track_config_t *config,
                                      h2_pal_audio_track_t **out_track) {
  h2_android_platform_t *host = user;
  if (host == NULL || out_track == NULL ||
      h2_android_audio_validate_track_config(config) != H2_PAL_OK ||
      (config->allocator != NULL &&
       (config->allocator->vtable == NULL ||
        config->allocator->vtable->alloc == NULL ||
        config->allocator->vtable->free == NULL))) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  const int can_create = host->speaker_started && host->audio_track == NULL;
  pthread_mutex_unlock(&host->mutex);
  if (!can_create) {
    return H2_AUDIO_ERR_INVALID_STATE;
  }

  AAudioStreamBuilder *builder = NULL;
  AAudioStream *stream = NULL;
  const int builder_result = h2_android_audio_map_aaudio_open_result(
      AAudio_createStreamBuilder(&builder));
  if (builder_result != H2_PAL_OK || builder == NULL) {
    return builder_result != H2_PAL_OK ? builder_result
                                       : H2_AUDIO_ERR_UNAVAILABLE;
  }
  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSampleRate(builder,
                                    (int32_t)config->format.sample_rate_hz);
  AAudioStreamBuilder_setChannelCount(builder,
                                      (int32_t)config->format.channels);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setPerformanceMode(builder,
                                         AAUDIO_PERFORMANCE_MODE_NONE);
  AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
  AAudioStreamBuilder_setBufferCapacityInFrames(
      builder, (int32_t)(config->buffer_frames *
                         config->format.frame_samples_per_channel));
  const aaudio_result_t open_result =
      AAudioStreamBuilder_openStream(builder, &stream);
  (void)AAudioStreamBuilder_delete(builder);
  const int mapped_open_result =
      h2_android_audio_map_aaudio_open_result(open_result);
  if (mapped_open_result != H2_PAL_OK || stream == NULL ||
      AAudioStream_getSampleRate(stream) !=
          (int32_t)config->format.sample_rate_hz ||
      AAudioStream_getChannelCount(stream) != config->format.channels ||
      AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_I16) {
    if (stream != NULL) {
      (void)AAudioStream_close(stream);
    }
    return mapped_open_result != H2_PAL_OK ? mapped_open_result
                                           : H2_AUDIO_ERR_UNSUPPORTED;
  }
  h2_android_audio_track_t *track = config->allocator != NULL
      ? h2_pal_mem_alloc(config->allocator, sizeof(*track))
      : malloc(sizeof(*track));
  if (track == NULL) {
    (void)AAudioStream_close(stream);
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  memset(track, 0, sizeof(*track));
  if (config->allocator != NULL) track->allocator = *config->allocator;
  track->base = (h2_pal_audio_track_t){
      .user = track,
      .audio = &host->audio,
      .write = android_audio_track_write,
      .close = android_audio_track_close,
      .get_volume_factor = android_audio_track_get_volume_factor,
      .set_volume_factor = android_audio_track_set_volume_factor,
      .drain = android_audio_track_drain,
  };
  track->host = host;
  track->stream = stream;
  track->format = config->format;
  track->volume_factor_milli = config->volume_factor_milli;
  /* AAudio requires its output buffer to be primed before starting. Fill it
   * with silence, so a single caller PCM frame can play and drain without
   * another caller write. Preserve the requested native buffer capacity. */
  const int32_t prime_frames = AAudioStream_getBufferSizeInFrames(stream);
  if (prime_frames <= 0 || (size_t)prime_frames >
      SIZE_MAX / (sizeof(int16_t) * config->format.channels)) {
    (void)AAudioStream_close(stream);
    android_track_free(track, track);
    return H2_AUDIO_ERR_IO;
  }
  const size_t prime_bytes = (size_t)prime_frames *
      config->format.channels * sizeof(int16_t);
  int16_t *silence = android_track_alloc(track, prime_bytes);
  if (silence == NULL) {
    (void)AAudioStream_close(stream);
    android_track_free(track, track);
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  memset(silence, 0, prime_bytes);
  int32_t primed = 0;
  int prime_result = H2_PAL_OK;
  while (primed < prime_frames) {
    const aaudio_result_t written = AAudioStream_write(
        stream, silence + (size_t)primed * config->format.channels,
        prime_frames - primed, 0);
    if (written <= 0 || written > prime_frames - primed) {
      prime_result = written == 0 ? H2_AUDIO_ERR_WOULD_BLOCK :
          written < 0 ? h2_android_audio_map_aaudio_write_result(
              written, AAUDIO_ERROR_TIMEOUT) : H2_AUDIO_ERR_IO;
      break;
    }
    primed += written;
  }
  android_track_free(track, silence);
  if (prime_result != H2_PAL_OK) {
    (void)AAudioStream_close(stream);
    android_track_free(track, track);
    return prime_result;
  }
  const int start_result = h2_android_audio_map_aaudio_io_result(
      AAudioStream_requestStart(stream));
  if (start_result != H2_PAL_OK) {
    (void)AAudioStream_close(stream);
    android_track_free(track, track);
    return start_result;
  }
  pthread_mutex_lock(&host->mutex);
  host->audio_track = track;
  pthread_mutex_unlock(&host->mutex);
  *out_track = &track->base;
  return H2_PAL_OK;
}

static int android_audio_get_speaker_volume(void *user,
                                            uint32_t *out_percent) {
  h2_android_platform_t *host = user;
  if (host == NULL || out_percent == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  *out_percent = host->speaker_volume_percent;
  pthread_mutex_unlock(&host->mutex);
  return H2_PAL_OK;
}

static int android_audio_set_speaker_volume(void *user, uint32_t percent) {
  h2_android_platform_t *host = user;
  if (host == NULL || percent > 100u) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  host->speaker_volume_percent = percent;
  pthread_mutex_unlock(&host->mutex);
  return H2_PAL_OK;
}

static int android_audio_get_mic_gain(void *user, uint32_t *out_percent) {
  h2_android_platform_t *host = user;
  if (host == NULL || out_percent == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&host->mutex);
  *out_percent = host->mic_gain_percent;
  pthread_mutex_unlock(&host->mutex);
  return H2_AUDIO_OK;
}

static int android_audio_set_mic_gain(void *user, uint32_t percent) {
  h2_android_platform_t *host = user;
  if (host == NULL || percent > 100u) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&host->mutex);
  host->mic_gain_percent = percent;
  pthread_mutex_unlock(&host->mutex);
  return H2_AUDIO_OK;
}

static int audio_aec_diagnostics_unsupported(void *user,
    const h2_audio_aec_observer_t *observer) {
    (void)user;
    (void)observer;
    return H2_AUDIO_ERR_UNSUPPORTED;
}

static const h2_pal_audio_vtable_t s_android_audio_vtable = {
    .get_info = android_audio_get_info,
    .start_mic = android_audio_start_mic,
    .stop_mic = android_audio_stop_mic,
    .start_speaker = android_audio_start_speaker,
    .stop_speaker = android_audio_stop_speaker,
    .mic_read = android_audio_mic_read,
    .create_track = android_audio_create_track,
    .get_speaker_volume_percent = android_audio_get_speaker_volume,
    .set_speaker_volume_percent = android_audio_set_speaker_volume,
    .get_mic_gain_percent = android_audio_get_mic_gain,
    .set_mic_gain_percent = android_audio_set_mic_gain,
    .set_aec_observer = audio_aec_diagnostics_unsupported,
};

static int android_display_open(void *user) {
  h2_android_platform_t *host = user;
  if (host == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  if (host->rgba == NULL) {
    host->rgba = calloc((size_t)host->width * host->height, sizeof(uint32_t));
  }
  if (!host->opened) host->brightness_percent = 100u;
  host->opened = host->rgba != NULL;
  pthread_mutex_unlock(&host->mutex);
  return host->opened ? H2_DISPLAY_OK : H2_DISPLAY_ERR_NO_MEMORY;
}

static int android_display_get_info(void *user, h2_display_info_t *out_info) {
  if (user == NULL || out_info == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  if (!((h2_android_platform_t *)user)->opened) return H2_DISPLAY_ERR_INVALID_STATE;
  *out_info = (h2_display_info_t){
      .width = ((h2_android_platform_t *)user)->width,
      .height = ((h2_android_platform_t *)user)->height,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  return H2_DISPLAY_OK;
}

static int android_display_draw_bitmap(void *user,
                                       const h2_display_rect_t *rect,
                                       const void *pixels, size_t stride_bytes,
                                       h2_display_pixel_format_t format) {
  h2_android_platform_t *host = user;
  if (host && !host->opened) return H2_DISPLAY_ERR_INVALID_STATE;
  if (format != H2_DISPLAY_PIXEL_RGB565) return H2_DISPLAY_ERR_UNSUPPORTED;
  if (host == NULL || rect == NULL || pixels == NULL || rect->x < 0 || rect->y < 0 ||
      rect->width <= 0 || rect->height <= 0 ||
      (int64_t)rect->x + rect->width > host->width ||
      (int64_t)rect->y + rect->height > host->height ||
      stride_bytes < (size_t)rect->width * sizeof(uint16_t) ||
      ((size_t)rect->height - 1u) > (SIZE_MAX - (size_t)rect->width*2u) / stride_bytes) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  for (int y = 0; y < rect->height; ++y) {
    const uint8_t *source =
        (const uint8_t *)pixels + (size_t)y * stride_bytes;
    uint32_t *destination =
        host->rgba + (size_t)(rect->y + y) * host->width + rect->x;
    for (int x = 0; x < rect->width; ++x) {
      uint16_t pixel;
      memcpy(&pixel, source + (size_t)x*2u, 2u);
      const uint32_t red = ((pixel >> 11u) & 0x1fu) * 255u / 31u;
      const uint32_t green = ((pixel >> 5u) & 0x3fu) * 255u / 63u;
      const uint32_t blue = (pixel & 0x1fu) * 255u / 31u;
      destination[x] = red | (green << 8u) | (blue << 16u) | 0xff000000u;
    }
  }
  pthread_mutex_unlock(&host->mutex);
  return H2_DISPLAY_OK;
}

static int android_display_present(void *user) {
  h2_android_platform_t *host = user;
  if (host == NULL || host->view == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  if (!host->opened) return H2_DISPLAY_ERR_INVALID_STATE;
  JNIEnv *env = NULL;
  int attached = 0;
  if ((*host->vm)->GetEnv(host->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
    if ((*host->vm)->AttachCurrentThread(host->vm, &env, NULL) != JNI_OK) {
      return H2_DISPLAY_ERR_IO;
    }
    attached = 1;
  }
  (*env)->CallVoidMethod(env, host->view, host->request_frame);
  const int failed = (*env)->ExceptionCheck(env);
  if (failed) {
    (*env)->ExceptionClear(env);
  }
  if (attached) {
    (void)(*host->vm)->DetachCurrentThread(host->vm);
  }
  return failed ? H2_DISPLAY_ERR_IO : H2_DISPLAY_OK;
}

static int android_display_set_brightness(void *user, uint32_t percent) {
  h2_android_platform_t *host = user;
  if (!host) return H2_DISPLAY_ERR_INVALID_ARG;
  pthread_mutex_lock(&host->mutex);
  int rc = !host->opened ? H2_DISPLAY_ERR_INVALID_STATE :
      percent > 100u ? H2_DISPLAY_ERR_INVALID_ARG : H2_DISPLAY_OK;
  if (!rc) host->brightness_percent = percent;
  pthread_mutex_unlock(&host->mutex);
  return rc ? rc : android_display_present(host);
}

static int android_display_close(void *user) {
  h2_android_platform_t *host = user;
  if (host == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  free(host->rgba);
  host->rgba = NULL;
  host->opened = 0;
  pthread_mutex_unlock(&host->mutex);
  return H2_DISPLAY_OK;
}

static const h2_pal_display_vtable_t s_android_display_vtable = {
    .open = android_display_open,
    .get_info = android_display_get_info,
    .draw_bitmap = android_display_draw_bitmap,
    .present = android_display_present,
    .set_brightness_percent = android_display_set_brightness,
    .close = android_display_close,
};

h2_android_platform_t *
h2_android_platform_create(JNIEnv *env, jobject view,
                           const h2_android_platform_config_t *config) {
  if (env == NULL || view == NULL || config == NULL ||
      config->display_width <= 0 || config->display_height <= 0) {
    return NULL;
  }
  h2_android_platform_t *host = calloc(1u, sizeof(*host));
  if (host == NULL || pthread_mutex_init(&host->mutex, NULL) != 0) {
    free(host);
    return NULL;
  }
  if ((*env)->GetJavaVM(env, &host->vm) != JNI_OK) {
    (void)pthread_mutex_destroy(&host->mutex);
    free(host);
    return NULL;
  }
  host->view = (*env)->NewGlobalRef(env, view);
  jclass view_class = (*env)->GetObjectClass(env, view);
  host->request_frame =
      view_class == NULL
          ? NULL
          : (*env)->GetMethodID(env, view_class, "requestFrame", "()V");
  if (view_class != NULL) {
    (*env)->DeleteLocalRef(env, view_class);
  }
  if (host->view == NULL || host->request_frame == NULL) {
    if (host->view != NULL) {
      (*env)->DeleteGlobalRef(env, host->view);
    }
    (void)pthread_mutex_destroy(&host->mutex);
    free(host);
    return NULL;
  }
  host->display.user = host;
  host->display.vtable = &s_android_display_vtable;
  host->audio.user = host;
  host->audio.vtable = &s_android_audio_vtable;
  host->speaker_volume_percent = 100u;
  host->mic_gain_percent = 100u;
  host->width = config->display_width;
  host->height = config->display_height;
  return host;
}

void h2_android_platform_destroy(h2_android_platform_t *host) {
  if (host == NULL) {
    return;
  }
  if (host->audio_track != NULL) {
    (void)android_audio_track_close(&host->audio_track->base);
  }
  (void)android_audio_stop_mic(host);
  (void)android_display_close(host);
  JNIEnv *env = NULL;
  int attached = 0;
  if ((*host->vm)->GetEnv(host->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK &&
      (*host->vm)->AttachCurrentThread(host->vm, &env, NULL) == JNI_OK) {
    attached = 1;
  }
  if (env != NULL && host->view != NULL) {
    (*env)->DeleteGlobalRef(env, host->view);
  }
  if (attached) {
    (void)(*host->vm)->DetachCurrentThread(host->vm);
  }
  (void)pthread_mutex_destroy(&host->mutex);
  free(host);
}

void h2_android_platform_update_pointer(h2_android_platform_t *host, int32_t x,
                                        int32_t y, int pressed) {
  if (host == NULL) {
    return;
  }
  pthread_mutex_lock(&host->mutex);
  host->pointer_x = x;
  host->pointer_y = y;
  host->pointer_pressed = pressed;
  pthread_mutex_unlock(&host->mutex);
}

h2_pal_result_t h2_android_platform_read_pointer(void *user, int32_t *out_x,
                                                 int32_t *out_y,
                                                 int *out_pressed) {
  h2_android_platform_t *host = user;
  if (host == NULL || out_x == NULL || out_y == NULL || out_pressed == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  *out_x = host->pointer_x;
  *out_y = host->pointer_y;
  *out_pressed = host->pointer_pressed;
  pthread_mutex_unlock(&host->mutex);
  return H2_PAL_OK;
}

int h2_android_platform_copy_frame(h2_android_platform_t *host, JNIEnv *env,
                                   jobject bitmap) {
  AndroidBitmapInfo info = {0};
  void *pixels = NULL;
  if (host == NULL || env == NULL || bitmap == NULL ||
      AndroidBitmap_getInfo(env, bitmap, &info) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      info.width != (uint32_t)host->width ||
      info.height != (uint32_t)host->height ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
      AndroidBitmap_lockPixels(env, bitmap, &pixels) !=
          ANDROID_BITMAP_RESULT_SUCCESS) {
    return 0;
  }
  pthread_mutex_lock(&host->mutex);
  if (host->rgba != NULL) {
    for (uint32_t y = 0; y < info.height; ++y) {
      uint8_t *row = (uint8_t *)pixels + (size_t)y * info.stride;
      const uint8_t *source = (const uint8_t *)(host->rgba + (size_t)y*host->width);
      for (uint32_t x=0; x<info.width; ++x) {
        for (unsigned c=0; c<3; ++c)
          row[x*4u+c] = (uint8_t)((uint32_t)source[x*4u+c] * host->brightness_percent / 100u);
        row[x*4u+3u] = 255u;
      }
    }
  }
  pthread_mutex_unlock(&host->mutex);
  (void)AndroidBitmap_unlockPixels(env, bitmap);
  return host->rgba != NULL;
}

const h2_pal_audio_api_t *
h2_android_platform_audio_api(h2_android_platform_t *host) {
  return host == NULL ? NULL : &host->audio;
}

const h2_pal_display_api_t *
h2_android_platform_display_api(h2_android_platform_t *host) {
  return host == NULL ? NULL : &host->display;
}
