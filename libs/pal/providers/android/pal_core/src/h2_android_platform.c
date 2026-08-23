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

struct h2_pal_queue {
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
  const h2_pal_mem_api_t *allocator;
  size_t item_size;
  size_t item_count;
  size_t head;
  size_t count;
  int closed;
  uint8_t *items;
};

struct h2_pal_task {
  pthread_t thread;
  h2_pal_task_entry_t entry;
  void *context;
};

typedef struct h2_android_audio_track {
  h2_pal_audio_track_t base;
  struct h2_android_platform *host;
  AAudioStream *stream;
  h2_audio_pcm_format_t format;
  uint32_t volume_factor_milli;
  int16_t *pending_samples;
  uint32_t pending_frame_count;
  uint32_t pending_frame_offset;
} h2_android_audio_track_t;

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
  uint32_t speaker_volume_percent;
  int speaker_started;
  h2_pal_display_api_t display;
  int opened;
};

static void *android_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *android_realloc(void *user, void *pointer, size_t len) {
  (void)user;
  return realloc(pointer, len);
}

static void android_free(void *user, void *pointer) {
  (void)user;
  free(pointer);
}

static const h2_pal_mem_vtable_t s_android_mem_vtable = {
    .alloc = android_alloc,
    .realloc = android_realloc,
    .free = android_free,
};
static const h2_pal_mem_api_t s_android_mem = {
    .user = NULL,
    .vtable = &s_android_mem_vtable,
};

static h2_pal_result_t android_time_get_monotonic_ms(void *user,
                                                     uint64_t *out_ms) {
  (void)user;
  struct timespec value = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return H2_PAL_ERR_IO;
  }
  *out_ms = (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
  return H2_PAL_OK;
}

static h2_pal_result_t android_time_get_monotonic_us(void *user,
                                                     uint64_t *out_us) {
  (void)user;
  struct timespec value = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return H2_PAL_ERR_IO;
  }
  *out_us = (uint64_t)value.tv_sec * 1000000u +
            (uint64_t)value.tv_nsec / 1000u;
  return H2_PAL_OK;
}

static h2_pal_result_t android_time_sleep_ms(void *user, uint32_t ms) {
  (void)user;
  struct timespec remaining = {
      .tv_sec = (time_t)(ms / 1000u),
      .tv_nsec = (long)(ms % 1000u) * 1000000l,
  };
  while (nanosleep(&remaining, &remaining) != 0) {
    if (errno != EINTR) {
      return H2_PAL_ERR_IO;
    }
  }
  return H2_PAL_OK;
}

static const h2_pal_time_vtable_t s_android_time_vtable = {
    .get_monotonic_ms = android_time_get_monotonic_ms,
    .get_monotonic_us = android_time_get_monotonic_us,
    .sleep_ms = android_time_sleep_ms,
};
static const h2_pal_time_api_t s_android_time = {
    .user = NULL,
    .vtable = &s_android_time_vtable,
};

static int android_log_write(void *user, h2_pal_log_level_t level,
                             const char *scope, const char *message) {
  (void)user;
  static const int priorities[] = {
      ANDROID_LOG_DEBUG,
      ANDROID_LOG_INFO,
      ANDROID_LOG_WARN,
      ANDROID_LOG_ERROR,
  };
  if (level < H2_PAL_LOG_DEBUG || level > H2_PAL_LOG_ERROR ||
      message == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  (void)__android_log_print(priorities[level], "H2Firmwares", "%s: %s",
                            scope == NULL ? "app" : scope, message);
  return H2_PAL_OK;
}

static const h2_pal_log_vtable_t s_android_log_vtable = {
    .write = android_log_write,
};
static const h2_pal_log_api_t s_android_log = {
    .user = NULL,
    .vtable = &s_android_log_vtable,
};

static void *android_task_entry(void *user) {
  h2_pal_task_t *task = user;
  task->entry(task->context);
  return NULL;
}

static int android_task_start(void *user,
                              const h2_pal_task_options_t *options,
                              h2_pal_task_entry_t entry, void *context,
                              h2_pal_task_t **out_task) {
  (void)user;
  if (entry == NULL || out_task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_task = NULL;
  h2_pal_task_t *task = calloc(1u, sizeof(*task));
  if (task == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  task->entry = entry;
  task->context = context;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) {
    free(task);
    return H2_PAL_ERR_UNAVAILABLE;
  }
  size_t stack_size = 0u;
  if (options != NULL && options->min_stack_size != 0u &&
      (pthread_attr_getstacksize(&attributes, &stack_size) != 0 ||
       (options->min_stack_size > stack_size &&
        pthread_attr_setstacksize(&attributes, options->min_stack_size) !=
            0))) {
    (void)pthread_attr_destroy(&attributes);
    free(task);
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int create_result =
      pthread_create(&task->thread, &attributes, android_task_entry, task);
  (void)pthread_attr_destroy(&attributes);
  if (create_result != 0) {
    free(task);
    return H2_PAL_ERR_UNAVAILABLE;
  }
  *out_task = task;
  return H2_PAL_OK;
}

static int android_task_join(void *user, h2_pal_task_t *task) {
  (void)user;
  if (task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (pthread_join(task->thread, NULL) != 0) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  free(task);
  return H2_PAL_OK;
}

static const h2_pal_task_vtable_t s_android_task_vtable = {
    .start = android_task_start,
    .join = android_task_join,
};
static const h2_pal_task_api_t s_android_task = {
    .user = NULL,
    .vtable = &s_android_task_vtable,
};

static void queue_deadline(uint32_t timeout_ms, struct timespec *deadline) {
  (void)clock_gettime(CLOCK_REALTIME, deadline);
  deadline->tv_sec += (time_t)(timeout_ms / 1000u);
  deadline->tv_nsec += (long)(timeout_ms % 1000u) * 1000000l;
  if (deadline->tv_nsec >= 1000000000l) {
    ++deadline->tv_sec;
    deadline->tv_nsec -= 1000000000l;
  }
}

static int android_queue_create(void *user, const h2_pal_queue_config_t *config,
                                h2_pal_queue_t **out_queue) {
  (void)user;
  if (config == NULL || config->allocator == NULL || out_queue == NULL ||
      config->item_size == 0u || config->item_count == 0u ||
      config->item_size > SIZE_MAX / config->item_count) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  *out_queue = NULL;
  h2_pal_queue_t *queue = h2_pal_mem_alloc(config->allocator, sizeof(*queue));
  if (queue == NULL) {
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  memset(queue, 0, sizeof(*queue));
  queue->items = h2_pal_mem_alloc(config->allocator,
                                  config->item_size * config->item_count);
  if (queue->items == NULL || pthread_mutex_init(&queue->mutex, NULL) != 0) {
    h2_pal_mem_free(config->allocator, queue->items);
    h2_pal_mem_free(config->allocator, queue);
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
    (void)pthread_mutex_destroy(&queue->mutex);
    h2_pal_mem_free(config->allocator, queue->items);
    h2_pal_mem_free(config->allocator, queue);
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  if (pthread_cond_init(&queue->not_full, NULL) != 0) {
    (void)pthread_cond_destroy(&queue->not_empty);
    (void)pthread_mutex_destroy(&queue->mutex);
    h2_pal_mem_free(config->allocator, queue->items);
    h2_pal_mem_free(config->allocator, queue);
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  queue->allocator = config->allocator;
  queue->item_size = config->item_size;
  queue->item_count = config->item_count;
  *out_queue = queue;
  return H2_PAL_QUEUE_OK;
}

static void android_queue_destroy(void *user, h2_pal_queue_t *queue) {
  (void)user;
  if (queue == NULL) {
    return;
  }
  (void)pthread_cond_destroy(&queue->not_empty);
  (void)pthread_cond_destroy(&queue->not_full);
  (void)pthread_mutex_destroy(&queue->mutex);
  h2_pal_mem_free(queue->allocator, queue->items);
  h2_pal_mem_free(queue->allocator, queue);
}

static int android_queue_send(void *user, h2_pal_queue_t *queue,
                              const void *item, uint32_t timeout_ms) {
  (void)user;
  if (queue == NULL || item == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  if (timeout_ms == UINT32_MAX) {
    while (queue->count == queue->item_count && !queue->closed) {
      (void)pthread_cond_wait(&queue->not_full, &queue->mutex);
    }
  } else if (timeout_ms != 0u && queue->count == queue->item_count &&
             !queue->closed) {
    struct timespec deadline = {0};
    queue_deadline(timeout_ms, &deadline);
    while (queue->count == queue->item_count && !queue->closed &&
           pthread_cond_timedwait(&queue->not_full, &queue->mutex, &deadline) ==
               0) {
    }
  }
  if (queue->closed || queue->count == queue->item_count) {
    const int result =
        queue->closed
            ? H2_PAL_QUEUE_ERR_CLOSED
            : (timeout_ms == 0u ? H2_PAL_ERR_FULL : H2_PAL_QUEUE_ERR_TIMEOUT);
    pthread_mutex_unlock(&queue->mutex);
    return result;
  }
  const size_t tail = (queue->head + queue->count) % queue->item_count;
  memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
  ++queue->count;
  pthread_cond_signal(&queue->not_empty);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static int android_queue_send_latest(void *user, h2_pal_queue_t *queue,
                                     const void *item) {
  (void)user;
  if (queue == NULL || item == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  if (queue->closed) {
    pthread_mutex_unlock(&queue->mutex);
    return H2_PAL_QUEUE_ERR_CLOSED;
  }
  if (queue->count == queue->item_count) {
    queue->head = (queue->head + 1u) % queue->item_count;
    --queue->count;
  }
  const size_t tail = (queue->head + queue->count) % queue->item_count;
  memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
  ++queue->count;
  pthread_cond_signal(&queue->not_empty);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static int android_queue_recv(void *user, h2_pal_queue_t *queue, void *out_item,
                              uint32_t timeout_ms) {
  (void)user;
  if (queue == NULL || out_item == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  if (timeout_ms == UINT32_MAX) {
    while (queue->count == 0u && !queue->closed) {
      (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
  } else if (timeout_ms != 0u && queue->count == 0u && !queue->closed) {
    struct timespec deadline = {0};
    queue_deadline(timeout_ms, &deadline);
    while (queue->count == 0u && !queue->closed &&
           pthread_cond_timedwait(&queue->not_empty, &queue->mutex,
                                  &deadline) == 0) {
    }
  }
  if (queue->count == 0u) {
    const int result = queue->closed
                           ? H2_PAL_QUEUE_ERR_CLOSED
                           : (timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                               : H2_PAL_QUEUE_ERR_TIMEOUT);
    pthread_mutex_unlock(&queue->mutex);
    return result;
  }
  memcpy(out_item, queue->items + queue->head * queue->item_size,
         queue->item_size);
  queue->head = (queue->head + 1u) % queue->item_count;
  --queue->count;
  pthread_cond_signal(&queue->not_full);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static int android_queue_reset(void *user, h2_pal_queue_t *queue) {
  (void)user;
  if (queue == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  queue->head = 0u;
  queue->count = 0u;
  pthread_cond_broadcast(&queue->not_full);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static int android_queue_close(void *user, h2_pal_queue_t *queue) {
  (void)user;
  if (queue == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  queue->closed = 1;
  pthread_cond_broadcast(&queue->not_empty);
  pthread_cond_broadcast(&queue->not_full);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static const h2_pal_queue_vtable_t s_android_queue_vtable = {
    .create = android_queue_create,
    .destroy = android_queue_destroy,
    .send = android_queue_send,
    .send_latest = android_queue_send_latest,
    .recv = android_queue_recv,
    .reset = android_queue_reset,
    .close = android_queue_close,
};
static const h2_pal_queue_api_t s_android_queue = {
    .user = NULL,
    .vtable = &s_android_queue_vtable,
};

static int android_audio_get_info(void *user, h2_audio_info_t *out_info) {
  if (user == NULL || out_info == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  *out_info = (h2_audio_info_t){
      .available = 1,
      .mic_supported = 0,
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

static int android_audio_unsupported(void *user) {
  return user == NULL ? H2_AUDIO_ERR_INVALID_ARG : H2_AUDIO_ERR_UNSUPPORTED;
}

static int android_audio_mic_read(void *user, h2_audio_frame_t *out_frame,
                                  uint32_t timeout_ms) {
  (void)out_frame;
  (void)timeout_ms;
  return user == NULL ? H2_AUDIO_ERR_INVALID_ARG : H2_AUDIO_ERR_UNSUPPORTED;
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
  free(track->pending_samples);
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

  int16_t *samples = malloc(frame->bytes);
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
      free(samples);
      return result == 0
                 ? H2_AUDIO_ERR_WOULD_BLOCK
                 : h2_android_audio_map_aaudio_write_result(
                       result, AAUDIO_ERROR_TIMEOUT);
    }
    if ((uint32_t)result > frame->samples_per_channel - written) {
      free(samples);
      return H2_AUDIO_ERR_IO;
    }
    written += (uint32_t)result;
  }
  free(samples);
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
  free(track->pending_samples);
  pthread_mutex_lock(&host->mutex);
  if (host->audio_track == track) {
    host->audio_track = NULL;
  }
  pthread_mutex_unlock(&host->mutex);
  free(track);
  return h2_android_audio_map_aaudio_io_result(close_result);
}

static int android_audio_create_track(void *user,
                                      const h2_audio_track_config_t *config,
                                      h2_pal_audio_track_t **out_track) {
  h2_android_platform_t *host = user;
  if (host == NULL || out_track == NULL ||
      h2_android_audio_validate_track_config(config) != H2_PAL_OK) {
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
  h2_android_audio_track_t *track = calloc(1u, sizeof(*track));
  if (track == NULL) {
    (void)AAudioStream_close(stream);
    return H2_AUDIO_ERR_NO_MEMORY;
  }
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
  const int start_result = h2_android_audio_map_aaudio_io_result(
      AAudioStream_requestStart(stream));
  if (start_result != H2_PAL_OK) {
    (void)AAudioStream_close(stream);
    free(track);
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

static const h2_pal_audio_vtable_t s_android_audio_vtable = {
    .get_info = android_audio_get_info,
    .start_mic = android_audio_unsupported,
    .stop_mic = android_audio_unsupported,
    .start_speaker = android_audio_start_speaker,
    .stop_speaker = android_audio_stop_speaker,
    .mic_read = android_audio_mic_read,
    .create_track = android_audio_create_track,
    .get_speaker_volume_percent = android_audio_get_speaker_volume,
    .set_speaker_volume_percent = android_audio_set_speaker_volume,
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
  host->opened = host->rgba != NULL;
  pthread_mutex_unlock(&host->mutex);
  return host->opened ? H2_DISPLAY_OK : H2_DISPLAY_ERR_NO_MEMORY;
}

static int android_display_get_info(void *user, h2_display_info_t *out_info) {
  if (user == NULL || out_info == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
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
  if (host == NULL || rect == NULL || pixels == NULL || host->rgba == NULL ||
      format != H2_DISPLAY_PIXEL_RGB565 || rect->x < 0 || rect->y < 0 ||
      rect->width <= 0 || rect->height <= 0 ||
      rect->x + rect->width > host->width ||
      rect->y + rect->height > host->height ||
      stride_bytes < (size_t)rect->width * sizeof(uint16_t)) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  for (int y = 0; y < rect->height; ++y) {
    const uint16_t *source =
        (const uint16_t *)((const uint8_t *)pixels + (size_t)y * stride_bytes);
    uint32_t *destination =
        host->rgba + (size_t)(rect->y + y) * host->width + rect->x;
    for (int x = 0; x < rect->width; ++x) {
      const uint16_t pixel = source[x];
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
  return user != NULL && percent <= 100u ? H2_DISPLAY_OK
                                         : H2_DISPLAY_ERR_INVALID_ARG;
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
      memcpy((uint8_t *)pixels + (size_t)y * info.stride,
             host->rgba + (size_t)y * host->width,
             (size_t)host->width * sizeof(uint32_t));
    }
  }
  pthread_mutex_unlock(&host->mutex);
  (void)AndroidBitmap_unlockPixels(env, bitmap);
  return host->rgba != NULL;
}

const h2_pal_mem_api_t *h2_android_platform_mem_api(void) {
  return &s_android_mem;
}

const h2_pal_time_api_t *h2_android_platform_time_api(void) {
  return &s_android_time;
}

const h2_pal_task_api_t *h2_android_platform_task_api(void) {
  return &s_android_task;
}

const h2_pal_queue_api_t *h2_android_platform_queue_api(void) {
  return &s_android_queue;
}

const h2_pal_log_api_t *h2_android_platform_log_api(void) {
  return &s_android_log;
}

const h2_pal_audio_api_t *
h2_android_platform_audio_api(h2_android_platform_t *host) {
  return host == NULL ? NULL : &host->audio;
}

const h2_pal_display_api_t *
h2_android_platform_display_api(h2_android_platform_t *host) {
  return host == NULL ? NULL : &host->display;
}
