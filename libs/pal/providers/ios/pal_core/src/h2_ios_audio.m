#import "h2_ios_platform.h"
#include "h2_ios_audio_wait.h"

#import <AVFoundation/AVFoundation.h>
#include <AudioToolbox/AudioToolbox.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { H2_IOS_AUDIO_MIC_SAMPLES = 320, H2_IOS_AUDIO_PLAY_SAMPLES = 512,
       H2_IOS_AUDIO_QUEUE_BUFFERS = 4, H2_IOS_AUDIO_MIC_FRAMES = 8 };

typedef struct h2_ios_audio_track {
  h2_pal_audio_track_t base;
  struct h2_ios_audio *owner;
  uint32_t volume_factor_milli;
  h2_pal_mem_api_t allocator;
} h2_ios_audio_track_t;

static void release_track(h2_ios_audio_track_t *track) {
  if (track == NULL) return;
  const h2_pal_mem_api_t allocator = track->allocator;
  if (allocator.vtable != NULL) h2_pal_mem_free(&allocator, track);
  else free(track);
}

struct h2_ios_audio {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  h2_pal_audio_api_t api;
  AudioQueueRef input_queue;
  AudioQueueRef output_queue;
  AudioQueueBufferRef output_buffers[H2_IOS_AUDIO_QUEUE_BUFFERS];
  int output_free[H2_IOS_AUDIO_QUEUE_BUFFERS];
  unsigned output_pending;
  h2_ios_audio_track_t *track;
  int mic_started;
  int speaker_started;
  int16_t mic_frames[H2_IOS_AUDIO_MIC_FRAMES][H2_IOS_AUDIO_MIC_SAMPLES];
  unsigned mic_head;
  unsigned mic_count;
  uint32_t mic_gain_percent;
  uint32_t speaker_volume_percent;
};

static AudioStreamBasicDescription pcm_format(void) {
  AudioStreamBasicDescription format = {0};
  format.mSampleRate = 16000.0;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  format.mBytesPerPacket = sizeof(int16_t);
  format.mFramesPerPacket = 1u;
  format.mBytesPerFrame = sizeof(int16_t);
  format.mChannelsPerFrame = 1u;
  format.mBitsPerChannel = 16u;
  return format;
}

static int wait_changed(h2_ios_audio_t *owner,
                         const h2_ios_audio_wait_budget_t *budget) {
  return h2_ios_audio_wait_changed(&owner->changed, &owner->mutex, budget);
}

static void input_callback(void *user, AudioQueueRef queue,
                           AudioQueueBufferRef buffer,
                           const AudioTimeStamp *start_time,
                           UInt32 packet_count,
                           const AudioStreamPacketDescription *packets) {
  (void)start_time;
  (void)packet_count;
  (void)packets;
  h2_ios_audio_t *owner = user;
  pthread_mutex_lock(&owner->mutex);
  if (owner->mic_started && owner->input_queue == queue) {
    if (buffer->mAudioDataByteSize >= H2_IOS_AUDIO_MIC_SAMPLES * sizeof(int16_t)) {
      if (owner->mic_count < H2_IOS_AUDIO_MIC_FRAMES) {
        const unsigned tail = (owner->mic_head + owner->mic_count) % H2_IOS_AUDIO_MIC_FRAMES;
        memcpy(owner->mic_frames[tail], buffer->mAudioData,
               H2_IOS_AUDIO_MIC_SAMPLES * sizeof(int16_t));
        ++owner->mic_count;
        pthread_cond_broadcast(&owner->changed);
      }
    }
    (void)AudioQueueEnqueueBuffer(queue, buffer, 0u, NULL);
  }
  pthread_mutex_unlock(&owner->mutex);
}

static void output_callback(void *user, AudioQueueRef queue,
                            AudioQueueBufferRef buffer) {
  h2_ios_audio_t *owner = user;
  pthread_mutex_lock(&owner->mutex);
  if (owner->output_queue == queue) {
    for (unsigned i = 0u; i < H2_IOS_AUDIO_QUEUE_BUFFERS; ++i) {
      if (owner->output_buffers[i] == buffer) {
        owner->output_free[i] = 1;
        if (owner->output_pending > 0u) --owner->output_pending;
        pthread_cond_broadcast(&owner->changed);
        break;
      }
    }
  }
  pthread_mutex_unlock(&owner->mutex);
}

static int activate_session(void) {
  NSError *error = nil;
  AVAudioSession *session = AVAudioSession.sharedInstance;
  if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                      mode:AVAudioSessionModeDefault
                   options:AVAudioSessionCategoryOptionDefaultToSpeaker
                     error:&error] ||
      ![session setPreferredSampleRate:16000.0 error:&error] ||
      ![session setActive:YES error:&error])
    return H2_AUDIO_ERR_UNAVAILABLE;
  return H2_AUDIO_OK;
}

static int audio_get_info(void *user, h2_audio_info_t *info) {
  if (user == NULL || info == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  *info = (h2_audio_info_t){
      .available = 1, .mic_supported = 1, .playback_supported = 1,
      .mic_format = {16000u, H2_IOS_AUDIO_MIC_SAMPLES, 1u, H2_AUDIO_SAMPLE_S16LE},
      .playback_format = {16000u, H2_IOS_AUDIO_PLAY_SAMPLES, 1u, H2_AUDIO_SAMPLE_S16LE},
      .mic_queue_frames = H2_IOS_AUDIO_MIC_FRAMES,
      .track_queue_frames = H2_IOS_AUDIO_QUEUE_BUFFERS,
      .max_tracks = 1u};
  return H2_AUDIO_OK;
}

static int audio_start_mic(void *user) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  if (owner->mic_started) {
    pthread_mutex_unlock(&owner->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  pthread_mutex_unlock(&owner->mutex);
  int rc = activate_session();
  if (rc != H2_AUDIO_OK) return rc;
  AudioQueueRef queue = NULL;
  AudioStreamBasicDescription format = pcm_format();
  if (AudioQueueNewInput(&format, input_callback, owner, NULL, NULL, 0u,
                         &queue) != noErr || queue == NULL)
    return H2_AUDIO_ERR_UNAVAILABLE;
  for (unsigned i = 0u; i < H2_IOS_AUDIO_QUEUE_BUFFERS; ++i) {
    AudioQueueBufferRef buffer = NULL;
    if (AudioQueueAllocateBuffer(queue, H2_IOS_AUDIO_MIC_SAMPLES * sizeof(int16_t),
                                  &buffer) != noErr ||
        AudioQueueEnqueueBuffer(queue, buffer, 0u, NULL) != noErr) {
      (void)AudioQueueDispose(queue, true);
      return H2_AUDIO_ERR_IO;
    }
  }
  pthread_mutex_lock(&owner->mutex);
  owner->input_queue = queue;
  owner->mic_head = owner->mic_count = 0u;
  owner->mic_started = 1;
  pthread_mutex_unlock(&owner->mutex);
  if (AudioQueueStart(queue, NULL) != noErr) {
    pthread_mutex_lock(&owner->mutex);
    owner->mic_started = 0;
    owner->input_queue = NULL;
    pthread_mutex_unlock(&owner->mutex);
    (void)AudioQueueDispose(queue, true);
    return H2_AUDIO_ERR_UNAVAILABLE;
  }
  return H2_AUDIO_OK;
}

static int audio_stop_mic(void *user) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  AudioQueueRef queue = owner->input_queue;
  owner->input_queue = NULL;
  owner->mic_started = 0;
  owner->mic_count = 0u;
  pthread_cond_broadcast(&owner->changed);
  pthread_mutex_unlock(&owner->mutex);
  if (queue == NULL) return H2_AUDIO_OK;
  const OSStatus stop = AudioQueueStop(queue, true);
  const OSStatus dispose = AudioQueueDispose(queue, true);
  return stop == noErr && dispose == noErr ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}

static int audio_start_speaker(void *user) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  if (owner->speaker_started) {
    pthread_mutex_unlock(&owner->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  pthread_mutex_unlock(&owner->mutex);
  int rc = activate_session();
  if (rc != H2_AUDIO_OK) return rc;
  AudioStreamBasicDescription format = pcm_format();
  AudioQueueRef queue = NULL;
  if (AudioQueueNewOutput(&format, output_callback, owner, NULL, NULL, 0u,
                          &queue) != noErr || queue == NULL)
    return H2_AUDIO_ERR_UNAVAILABLE;
  AudioQueueBufferRef buffers[H2_IOS_AUDIO_QUEUE_BUFFERS] = {0};
  for (unsigned i = 0u; i < H2_IOS_AUDIO_QUEUE_BUFFERS; ++i) {
    if (AudioQueueAllocateBuffer(queue, H2_IOS_AUDIO_PLAY_SAMPLES * sizeof(int16_t),
                                  &buffers[i]) != noErr) {
      (void)AudioQueueDispose(queue, true);
      return H2_AUDIO_ERR_NO_MEMORY;
    }
  }
  pthread_mutex_lock(&owner->mutex);
  owner->output_queue = queue;
  owner->output_pending = 0u;
  for (unsigned i = 0u; i < H2_IOS_AUDIO_QUEUE_BUFFERS; ++i) {
    owner->output_buffers[i] = buffers[i];
    owner->output_free[i] = 1;
  }
  owner->speaker_started = 1;
  pthread_mutex_unlock(&owner->mutex);
  if (AudioQueueStart(queue, NULL) != noErr) {
    pthread_mutex_lock(&owner->mutex);
    owner->speaker_started = 0;
    owner->output_queue = NULL;
    pthread_mutex_unlock(&owner->mutex);
    (void)AudioQueueDispose(queue, true);
    return H2_AUDIO_ERR_UNAVAILABLE;
  }
  return H2_AUDIO_OK;
}

static int audio_stop_speaker(void *user) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  if (owner->track != NULL) {
    pthread_mutex_unlock(&owner->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  AudioQueueRef queue = owner->output_queue;
  owner->output_queue = NULL;
  owner->speaker_started = 0;
  owner->output_pending = 0u;
  pthread_cond_broadcast(&owner->changed);
  pthread_mutex_unlock(&owner->mutex);
  if (queue == NULL) return H2_AUDIO_OK;
  const OSStatus stop = AudioQueueStop(queue, true);
  const OSStatus dispose = AudioQueueDispose(queue, true);
  return stop == noErr && dispose == noErr ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}

static int audio_mic_read(void *user, h2_audio_frame_t *frame, uint32_t timeout_ms) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL || frame == NULL || frame->data == NULL ||
      frame->capacity < H2_IOS_AUDIO_MIC_SAMPLES * sizeof(int16_t) ||
      frame->sample_rate_hz != 16000u || frame->channels != 1u ||
      frame->sample_format != H2_AUDIO_SAMPLE_S16LE)
    return H2_AUDIO_ERR_INVALID_ARG;
  h2_ios_audio_wait_budget_t budget;
  const int budget_rc = h2_ios_audio_wait_begin(timeout_ms, &budget);
  if (budget_rc != H2_AUDIO_OK) return budget_rc;
  pthread_mutex_lock(&owner->mutex);
  if (!owner->mic_started) {
    pthread_mutex_unlock(&owner->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  while (owner->mic_count == 0u && owner->mic_started) {
    const int rc = wait_changed(owner, &budget);
    if (rc != H2_AUDIO_OK) {
      pthread_mutex_unlock(&owner->mutex);
      return rc;
    }
  }
  if (!owner->mic_started) {
    pthread_mutex_unlock(&owner->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  const int16_t *source = owner->mic_frames[owner->mic_head];
  int16_t *destination = frame->data;
  for (unsigned i = 0u; i < H2_IOS_AUDIO_MIC_SAMPLES; ++i)
    destination[i] = (int16_t)((int32_t)source[i] *
                              (int32_t)owner->mic_gain_percent / 100);
  owner->mic_head = (owner->mic_head + 1u) % H2_IOS_AUDIO_MIC_FRAMES;
  --owner->mic_count;
  pthread_mutex_unlock(&owner->mutex);
  frame->bytes = H2_IOS_AUDIO_MIC_SAMPLES * sizeof(int16_t);
  frame->samples_per_channel = H2_IOS_AUDIO_MIC_SAMPLES;
  return H2_AUDIO_OK;
}

static int track_drain(h2_pal_audio_track_t *base, uint32_t timeout_ms) {
  h2_ios_audio_track_t *track = (h2_ios_audio_track_t *)base;
  if (track == NULL || track->owner == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  h2_ios_audio_t *owner = track->owner;
  h2_ios_audio_wait_budget_t budget;
  const int budget_rc = h2_ios_audio_wait_begin(timeout_ms, &budget);
  if (budget_rc != H2_AUDIO_OK) return budget_rc;
  pthread_mutex_lock(&owner->mutex);
  while (owner->output_pending != 0u && owner->speaker_started) {
    const int rc = wait_changed(owner, &budget);
    if (rc != H2_AUDIO_OK) {
      pthread_mutex_unlock(&owner->mutex);
      return rc;
    }
  }
  const int rc = owner->speaker_started ? H2_AUDIO_OK : H2_AUDIO_ERR_INVALID_STATE;
  pthread_mutex_unlock(&owner->mutex);
  return rc;
}

static int track_write(h2_pal_audio_track_t *base, const h2_audio_frame_t *frame,
                       uint32_t timeout_ms) {
  h2_ios_audio_track_t *track = (h2_ios_audio_track_t *)base;
  if (track == NULL || track->owner == NULL || frame == NULL || frame->data == NULL ||
      frame->sample_rate_hz != 16000u || frame->channels != 1u ||
      frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      frame->samples_per_channel != H2_IOS_AUDIO_PLAY_SAMPLES ||
      frame->bytes != H2_IOS_AUDIO_PLAY_SAMPLES * sizeof(int16_t))
    return H2_AUDIO_ERR_INVALID_ARG;
  h2_ios_audio_t *owner = track->owner;
  h2_ios_audio_wait_budget_t budget;
  const int budget_rc = h2_ios_audio_wait_begin(timeout_ms, &budget);
  if (budget_rc != H2_AUDIO_OK) return budget_rc;
  pthread_mutex_lock(&owner->mutex);
  unsigned index = 0u;
  for (;;) {
    if (!owner->speaker_started || owner->track != track) {
      pthread_mutex_unlock(&owner->mutex);
      return H2_AUDIO_ERR_INVALID_STATE;
    }
    for (index = 0u; index < H2_IOS_AUDIO_QUEUE_BUFFERS; ++index)
      if (owner->output_free[index]) break;
    if (index < H2_IOS_AUDIO_QUEUE_BUFFERS) break;
    const int rc = wait_changed(owner, &budget);
    if (rc != H2_AUDIO_OK) {
      pthread_mutex_unlock(&owner->mutex);
      return rc;
    }
  }
  AudioQueueBufferRef buffer = owner->output_buffers[index];
  const int16_t *source = frame->data;
  int16_t *destination = buffer->mAudioData;
  const uint32_t factor = track->volume_factor_milli;
  const uint32_t percent = owner->speaker_volume_percent;
  for (unsigned i = 0u; i < H2_IOS_AUDIO_PLAY_SAMPLES; ++i) {
    const int64_t scaled = (int64_t)source[i] * factor * percent / 100000u;
    destination[i] = (int16_t)scaled;
  }
  buffer->mAudioDataByteSize = H2_IOS_AUDIO_PLAY_SAMPLES * sizeof(int16_t);
  owner->output_free[index] = 0;
  ++owner->output_pending;
  const OSStatus rc = AudioQueueEnqueueBuffer(owner->output_queue, buffer, 0u, NULL);
  if (rc != noErr) {
    owner->output_free[index] = 1;
    --owner->output_pending;
  }
  pthread_mutex_unlock(&owner->mutex);
  return rc == noErr ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}

static int track_get_volume(h2_pal_audio_track_t *base, uint32_t *out) {
  h2_ios_audio_track_t *track = (h2_ios_audio_track_t *)base;
  if (track == NULL || track->owner == NULL || out == NULL)
    return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&track->owner->mutex);
  *out = track->volume_factor_milli;
  pthread_mutex_unlock(&track->owner->mutex);
  return H2_AUDIO_OK;
}

static int track_set_volume(h2_pal_audio_track_t *base, uint32_t value) {
  h2_ios_audio_track_t *track = (h2_ios_audio_track_t *)base;
  if (track == NULL || track->owner == NULL || value > 1000u)
    return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&track->owner->mutex);
  track->volume_factor_milli = value;
  pthread_mutex_unlock(&track->owner->mutex);
  return H2_AUDIO_OK;
}

static int track_close(h2_pal_audio_track_t *base) {
  h2_ios_audio_track_t *track = (h2_ios_audio_track_t *)base;
  if (track == NULL || track->owner == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  const int rc = track_drain(base, 3000u);
  if (rc != H2_AUDIO_OK) return rc;
  h2_ios_audio_t *owner = track->owner;
  pthread_mutex_lock(&owner->mutex);
  if (owner->track != track) {
    pthread_mutex_unlock(&owner->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  owner->track = NULL;
  pthread_mutex_unlock(&owner->mutex);
  release_track(track);
  return H2_AUDIO_OK;
}

static int audio_create_track(void *user, const h2_audio_track_config_t *config,
                              h2_pal_audio_track_t **out) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL || config == NULL || out == NULL ||
      config->format.sample_rate_hz != 16000u ||
      config->format.frame_samples_per_channel != H2_IOS_AUDIO_PLAY_SAMPLES ||
      config->format.channels != 1u ||
      config->format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      config->volume_factor_milli > 1000u ||
      (config->allocator != NULL &&
       (config->allocator->vtable == NULL ||
        config->allocator->vtable->alloc == NULL ||
        config->allocator->vtable->free == NULL)))
    return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  if (owner->track != NULL || !owner->speaker_started) {
    pthread_mutex_unlock(&owner->mutex);
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  h2_ios_audio_track_t *track = config->allocator != NULL
      ? h2_pal_mem_alloc(config->allocator, sizeof(*track))
      : malloc(sizeof(*track));
  if (track == NULL) {
    pthread_mutex_unlock(&owner->mutex);
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  memset(track, 0, sizeof(*track));
  if (config->allocator != NULL) track->allocator = *config->allocator;
  track->base = (h2_pal_audio_track_t){
      .user = track, .audio = &owner->api,
      .write = track_write, .close = track_close,
      .get_volume_factor = track_get_volume,
      .set_volume_factor = track_set_volume, .drain = track_drain};
  track->owner = owner;
  track->volume_factor_milli = config->volume_factor_milli;
  owner->track = track;
  *out = &track->base;
  pthread_mutex_unlock(&owner->mutex);
  return H2_AUDIO_OK;
}

static int audio_get_speaker_volume(void *user, uint32_t *out) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL || out == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  *out = owner->speaker_volume_percent;
  pthread_mutex_unlock(&owner->mutex);
  return H2_AUDIO_OK;
}
static int audio_set_speaker_volume(void *user, uint32_t value) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL || value > 100u) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  owner->speaker_volume_percent = value;
  pthread_mutex_unlock(&owner->mutex);
  return H2_AUDIO_OK;
}
static int audio_get_mic_gain(void *user, uint32_t *out) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL || out == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  *out = owner->mic_gain_percent;
  pthread_mutex_unlock(&owner->mutex);
  return H2_AUDIO_OK;
}
static int audio_set_mic_gain(void *user, uint32_t value) {
  h2_ios_audio_t *owner = user;
  if (owner == NULL || value > 100u) return H2_AUDIO_ERR_INVALID_ARG;
  pthread_mutex_lock(&owner->mutex);
  owner->mic_gain_percent = value;
  pthread_mutex_unlock(&owner->mutex);
  return H2_AUDIO_OK;
}

static int audio_aec_diagnostics_unsupported(void *user,
    const h2_audio_aec_observer_t *observer) {
    (void)user;
    (void)observer;
    return H2_AUDIO_ERR_UNSUPPORTED;
}

static const h2_pal_audio_vtable_t audio_vtable = {
    .get_info = audio_get_info,
    .start_mic = audio_start_mic, .stop_mic = audio_stop_mic,
    .start_speaker = audio_start_speaker, .stop_speaker = audio_stop_speaker,
    .mic_read = audio_mic_read, .create_track = audio_create_track,
    .get_speaker_volume_percent = audio_get_speaker_volume,
    .set_speaker_volume_percent = audio_set_speaker_volume,
    .get_mic_gain_percent = audio_get_mic_gain,
    .set_mic_gain_percent = audio_set_mic_gain,
    .set_aec_observer = audio_aec_diagnostics_unsupported,
};

h2_pal_result_t h2_ios_audio_create(h2_ios_audio_t **out_audio) {
  if (out_audio == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_audio = NULL;
  h2_ios_audio_t *owner = calloc(1u, sizeof(*owner));
  if (owner == NULL) return H2_PAL_ERR_NO_MEMORY;
  if (pthread_mutex_init(&owner->mutex, NULL) != 0) {
    free(owner);
    return H2_PAL_ERR_IO;
  }
  if (pthread_cond_init(&owner->changed, NULL) != 0) {
    (void)pthread_mutex_destroy(&owner->mutex);
    free(owner);
    return H2_PAL_ERR_IO;
  }
  owner->api = (h2_pal_audio_api_t){.user = owner, .vtable = &audio_vtable};
  owner->mic_gain_percent = owner->speaker_volume_percent = 100u;
  *out_audio = owner;
  return H2_PAL_OK;
}

const h2_pal_audio_api_t *h2_ios_audio_api(h2_ios_audio_t *audio) {
  return audio == NULL ? NULL : &audio->api;
}

void h2_ios_audio_destroy(h2_ios_audio_t *audio) {
  if (audio == NULL) return;
  (void)audio_stop_mic(audio);
  pthread_mutex_lock(&audio->mutex);
  h2_ios_audio_track_t *track = audio->track;
  AudioQueueRef output = audio->output_queue;
  audio->track = NULL;
  audio->output_queue = NULL;
  audio->speaker_started = 0;
  audio->output_pending = 0u;
  pthread_cond_broadcast(&audio->changed);
  pthread_mutex_unlock(&audio->mutex);
  if (output != NULL) {
    (void)AudioQueueStop(output, true);
    (void)AudioQueueDispose(output, true);
  }
  release_track(track);
  (void)pthread_cond_destroy(&audio->changed);
  (void)pthread_mutex_destroy(&audio->mutex);
  free(audio);
}
