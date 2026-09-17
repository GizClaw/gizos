#include "app_config.h"

#if defined(CONFIG_AUDIO_ENABLE) && CONFIG_AUDIO_ENABLE

#include "os/os_api.h"
#include "system/sys_time.h"
#include "server/audio_server.h"
#include "server/server_core.h"

#include "h2_jieli_ac791n_devkit.h"

#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"
#include "h2_jieli_wl82_atomic.h"

#include <stdlib.h>
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


/* One task-context gate protects lifecycle, ring contents and predicates. SDK
 * calls run outside it: OPEN/START/STOP may synchronously enter our VFS. */
enum audio_lifecycle { AUDIO_FREE, AUDIO_OPENING, AUDIO_OPEN, AUDIO_CLOSING };
typedef struct audio_ring {
  uint8_t *data;
  uint32_t capacity;
  uint32_t head;
  uint32_t count;
} audio_ring_t;
typedef struct jieli_audio_track {
  h2_pal_audio_track_t pal;
  struct server *server;
  enum audio_lifecycle state;
  uintptr_t generation;
  unsigned operations;
  audio_ring_t ring;
  uint64_t accepted;
  uint64_t consumed;
  uint32_t volume_factor_milli;
  int mic_monitor;
} jieli_audio_track_t;
typedef struct jieli_audio_state {
  volatile uint32_t initialized;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *changed;
  uintptr_t next_generation;
  struct server *mic_server;
  enum audio_lifecycle mic_state;
  uintptr_t mic_generation;
  audio_ring_t mic_ring;
  uint8_t mic_storage[H2_AUDIO_MIC_BUFFER_BYTES];
  int speaker_started;
  int speaker_stopping;
  uint32_t speaker_volume_percent;
  jieli_audio_track_t tracks[H2_AUDIO_MAX_TRACKS];
} jieli_audio_state_t;
static jieli_audio_state_t audio_state = {.speaker_volume_percent = 80u};

static void audio_stage(const char *message) {
  (void)h2_jieli_ac791n_devkit_console_write(message, strlen(message), 100u);
}
static uint32_t audio_remaining(uint32_t start, uint32_t budget) {
  if (budget == UINT32_MAX) return UINT32_MAX;
  uint32_t elapsed = timer_get_ms() - start;
  return elapsed < budget ? budget - elapsed : 0u;
}
static int audio_initialize(void) {
  for (;;) {
    uint32_t state = h2_jieli_atomic_load_u32(&audio_state.initialized);
    if (state == 2u) return H2_AUDIO_OK;
    uint32_t expected = 0u;
    if (state == 0u && h2_jieli_atomic_cas_u32(
            &audio_state.initialized, &expected, 1u)) {
      const h2_pal_sync_api_t *sync = h2_jieli_wl82_platform_sync_api();
      const h2_pal_mutex_config_t mutex_config = {.name = "audio-state"};
      const h2_pal_cond_config_t cond_config = {.name = "audio-changed"};
      int rc = h2_pal_mutex_create(sync, &mutex_config, &audio_state.mutex);
      if (rc == H2_PAL_OK) {
        rc = h2_pal_cond_create(sync, &cond_config, &audio_state.changed);
        if (rc != H2_PAL_OK) {
          (void)h2_pal_mutex_destroy(sync, audio_state.mutex);
          audio_state.mutex = NULL;
        }
      }
      h2_jieli_atomic_store_u32(&audio_state.initialized, rc == H2_PAL_OK ? 2u : 0u);
      return rc == H2_PAL_OK ? H2_AUDIO_OK : H2_AUDIO_ERR_NO_MEMORY;
    }
    h2_jieli_sdk_sleep_ms(1u);
  }
}
static int audio_lock(uint32_t start, uint32_t budget) {
  int rc = audio_initialize();
  if (rc != H2_AUDIO_OK) return rc;
  const h2_pal_sync_api_t *sync = h2_jieli_wl82_platform_sync_api();
  if (budget == UINT32_MAX) {
    return h2_pal_mutex_lock(sync, audio_state.mutex) == H2_PAL_OK
               ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
  }
  for (;;) {
    rc = h2_pal_mutex_try_lock(sync, audio_state.mutex);
    if (rc == H2_PAL_OK) return H2_AUDIO_OK;
    if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_BUSY) return H2_AUDIO_ERR_IO;
    if (audio_remaining(start, budget) == 0u) return H2_AUDIO_ERR_WOULD_BLOCK;
    h2_jieli_sdk_sleep_ms(1u);
  }
}
static void audio_unlock(void) {
  (void)h2_pal_mutex_unlock(h2_jieli_wl82_platform_sync_api(), audio_state.mutex);
}
static void audio_changed(void) {
  (void)h2_pal_cond_broadcast(h2_jieli_wl82_platform_sync_api(), audio_state.changed);
}
static int audio_wait(uint32_t start, uint32_t budget) {
  uint32_t remaining = audio_remaining(start, budget);
  if (remaining == 0u) return H2_AUDIO_ERR_WOULD_BLOCK;
  int locked = 1;
  int rc = h2_jieli_wl82_cond_wait_owned(
      audio_state.changed, audio_state.mutex, remaining, &locked);
  /* Retirement must own the gate even after an SDK relock error. Valid native
   * mutexes remain allocated for the provider's lifetime. */
  while (!locked) {
    locked = h2_pal_mutex_lock(h2_jieli_wl82_platform_sync_api(),
                              audio_state.mutex) == H2_PAL_OK;
    if (!locked) h2_jieli_sdk_sleep_ms(1u);
  }
  if (rc == H2_PAL_OK || rc == H2_PAL_ERR_TIMEOUT) return H2_AUDIO_OK;
  return H2_AUDIO_ERR_IO;
}
static uintptr_t audio_generation(void) {
  /* Never recycle a callback token, including on integer wrap. An obsolete SDK
   * callback can inspect static state but cannot reach a freed/reused ring. */
  if (audio_state.next_generation == UINTPTR_MAX) return 0u;
  return ++audio_state.next_generation;
}
static void ring_read(audio_ring_t *ring, void *data, uint32_t length) {
  uint32_t first = ring->capacity - ring->head;
  if (first > length) first = length;
  memcpy(data, ring->data + ring->head, first);
  memcpy((uint8_t *)data + first, ring->data, length - first);
  ring->head = (uint32_t)(((uint64_t)ring->head + length) % ring->capacity);
  ring->count -= length;
}
static void ring_write(audio_ring_t *ring, const void *data, uint32_t length) {
  uint32_t tail = (uint32_t)(((uint64_t)ring->head + ring->count) % ring->capacity);
  uint32_t first = ring->capacity - tail;
  if (first > length) first = length;
  memcpy(ring->data + tail, data, first);
  memcpy(ring->data, (const uint8_t *)data + first, length - first);
  ring->count += length;
}
static int mic_vfs_write(void *file, void *data, uint32_t length) {
  if (audio_lock(0u, UINT32_MAX) != H2_AUDIO_OK) return 0;
  if ((uintptr_t)file != audio_state.mic_generation ||
      (audio_state.mic_state != AUDIO_OPENING && audio_state.mic_state != AUDIO_OPEN)) {
    audio_unlock();
    return (int)length;
  }
  audio_ring_t *ring = &audio_state.mic_ring;
  /* Preserve whole PCM samples and keep the newest bounded window. */
  uint32_t kept = length & ~1u;
  const uint8_t *input = data;
  if (kept > ring->capacity) {
    input += kept - ring->capacity;
    kept = ring->capacity;
  }
  if (kept > ring->capacity - ring->count) {
    uint32_t discard = kept - (ring->capacity - ring->count);
    ring->head = (ring->head + discard) % ring->capacity;
    ring->count -= discard;
  }
  ring_write(ring, input, kept);
  audio_changed();
  audio_unlock();
  return (int)length;
}
static int audio_vfs_close(void *file) { (void)file; return 0; }
static int audio_vfs_length(void *file) { (void)file; return 0; }
static const struct audio_vfs_ops mic_vfs_ops = {
    .fwrite = mic_vfs_write, .fclose = audio_vfs_close, .flen = audio_vfs_length,
};
static int track_vfs_read(void *file, void *data, uint32_t length) {
  if (audio_lock(0u, UINT32_MAX) != H2_AUDIO_OK) return 0;
  jieli_audio_track_t *track = NULL;
  for (size_t i = 0u; i < H2_AUDIO_MAX_TRACKS; ++i) {
    if (audio_state.tracks[i].generation == (uintptr_t)file) {
      track = &audio_state.tracks[i];
      break;
    }
  }
  if (track == NULL || (track->state != AUDIO_OPEN && track->state != AUDIO_OPENING)) {
    audio_unlock();
    return 0;
  }
  ++track->operations;
  int result = 0;
  while (track->generation == (uintptr_t)file && track->state == AUDIO_OPEN && track->ring.count == 0u) {
    if (audio_wait(0u, UINT32_MAX) != H2_AUDIO_OK) goto finished;
  }
  if (track->generation != (uintptr_t)file || track->state == AUDIO_FREE || track->state == AUDIO_CLOSING) {
    goto finished;
  }
  if (track->ring.count == 0u) { result = -2; goto finished; }
  /* Match the SDK PCM helper's mono-to-stereo DAC conversion. */
  uint32_t count = (length / 2u) & ~1u;
  if (count > track->ring.count) count = track->ring.count;
  ring_read(&track->ring, data, count);
  track->consumed += count;
  int16_t *samples = data;
  for (uint32_t i = count / 2u; i > 0u; --i) {
    int16_t sample = samples[i - 1u];
    samples[2u * i - 1u] = sample;
    samples[2u * i - 2u] = sample;
  }
  result = (int)(count * 2u);
finished:
  /* STOP must not kill a decoder while its stack-owned condition waiter is
   * still linked. Retire VFS users alongside writers before any SDK teardown. */
  --track->operations;
  audio_changed();
  audio_unlock();
  return result;
}
static const struct audio_vfs_ops track_vfs_ops = {.fread = track_vfs_read};
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
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  while (audio_state.mic_state == AUDIO_OPENING || audio_state.mic_state == AUDIO_CLOSING) {
    if (audio_wait(0u, UINT32_MAX) != H2_AUDIO_OK) { audio_unlock(); return H2_AUDIO_ERR_IO; }
  }
  if (audio_state.mic_state == AUDIO_OPEN) { audio_unlock(); return H2_AUDIO_OK; }
  uintptr_t generation = audio_generation();
  if (generation == 0u) { audio_unlock(); return H2_AUDIO_ERR_UNAVAILABLE; }
  audio_state.mic_state = AUDIO_OPENING;
  audio_state.mic_generation = generation;
  audio_state.mic_ring = (audio_ring_t){.data = audio_state.mic_storage, .capacity = sizeof(audio_state.mic_storage)};
  audio_unlock();
  struct server *server = server_open("audio_server", "enc");
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
  request.enc.file = (FILE *)generation;
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

  rc = server == NULL ? H2_AUDIO_ERR_UNAVAILABLE :
      (server_request(server, AUDIO_REQ_ENC, &request) == 0 ? H2_AUDIO_OK : H2_AUDIO_ERR_IO);
  if (rc != H2_AUDIO_OK && server != NULL) server_close(server);
  (void)audio_lock(0u, UINT32_MAX);
  audio_state.mic_server = rc == H2_AUDIO_OK ? server : NULL;
  audio_state.mic_state = rc == H2_AUDIO_OK ? AUDIO_OPEN : AUDIO_FREE;
  audio_changed();
  audio_unlock();
  return rc;
}
static int audio_stop_mic(void *user) {
  (void)user;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  while (audio_state.mic_state == AUDIO_OPENING || audio_state.mic_state == AUDIO_CLOSING) {
    if (audio_wait(0u, UINT32_MAX) != H2_AUDIO_OK) { audio_unlock(); return H2_AUDIO_ERR_IO; }
  }
  if (audio_state.mic_state == AUDIO_FREE) { audio_unlock(); return H2_AUDIO_OK; }
  struct server *server = audio_state.mic_server;
  audio_state.mic_state = AUDIO_CLOSING;
  audio_changed();
  audio_unlock();
  union audio_req request = {0};
  request.enc.cmd = AUDIO_ENC_CLOSE;
  rc = server_request(server, AUDIO_REQ_ENC, &request);
  server_close(server);
  (void)audio_lock(0u, UINT32_MAX);
  audio_state.mic_server = NULL;
  audio_state.mic_state = AUDIO_FREE;
  audio_changed();
  audio_unlock();
  return rc == 0 ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}
static int audio_mic_read(void *user, h2_audio_frame_t *frame, uint32_t wait_ms) {
  (void)user;
  if (frame == NULL || frame->data == NULL || frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      frame->channels != 1u || frame->capacity < sizeof(int16_t)) return H2_AUDIO_ERR_INVALID_ARG;
  frame->bytes = 0u;
  uint32_t start = timer_get_ms();
  int rc = audio_lock(start, wait_ms);
  if (rc != H2_AUDIO_OK) return rc;
  uintptr_t generation = audio_state.mic_generation;
  for (;;) {
    if (audio_state.mic_state != AUDIO_OPEN || audio_state.mic_generation != generation) {
      rc = H2_AUDIO_ERR_INVALID_STATE;
      break;
    }
    if (audio_state.mic_ring.count != 0u) {
      uint32_t count = audio_state.mic_ring.count;
      if (count > H2_AUDIO_FRAME_BYTES) count = H2_AUDIO_FRAME_BYTES;
      if (count > frame->capacity) count = (uint32_t)frame->capacity;
      count &= ~1u;
      ring_read(&audio_state.mic_ring, frame->data, count);
      frame->bytes = count;
      frame->sample_rate_hz = H2_AUDIO_SAMPLE_RATE;
      frame->samples_per_channel = (uint16_t)(count / sizeof(int16_t));
      rc = H2_AUDIO_OK;
      break;
    }
    rc = audio_wait(start, wait_ms);
    if (rc != H2_AUDIO_OK) break;
  }
  audio_unlock();
  return rc;
}
static int audio_start_speaker(void *user) {
  (void)user;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  while (audio_state.speaker_stopping) {
    if (audio_wait(0u, UINT32_MAX) != H2_AUDIO_OK) { audio_unlock(); return H2_AUDIO_ERR_IO; }
  }
  audio_state.speaker_started = 1;
  audio_unlock();
  return H2_AUDIO_OK;
}
static int track_write(h2_pal_audio_track_t *pal_track, const h2_audio_frame_t *frame, uint32_t wait_ms) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  if (frame == NULL || frame->data == NULL || frame->bytes > UINT32_MAX || (frame->bytes & 1u) != 0u) return H2_AUDIO_ERR_INVALID_ARG;
  if (frame->sample_rate_hz != H2_AUDIO_SAMPLE_RATE || frame->channels != 1u ||
      frame->sample_format != H2_AUDIO_SAMPLE_S16LE) return H2_AUDIO_ERR_UNSUPPORTED;
  uint32_t start = timer_get_ms();
  int rc = audio_lock(start, wait_ms);
  if (rc != H2_AUDIO_OK) return rc;
  if (track->state != AUDIO_OPEN) { audio_unlock(); return H2_AUDIO_ERR_INVALID_STATE; }
  if (frame->bytes > track->ring.capacity) { audio_unlock(); return H2_AUDIO_ERR_INVALID_ARG; }
  ++track->operations;
  for (;;) {
    if (track->state != AUDIO_OPEN) { rc = H2_AUDIO_ERR_INVALID_STATE; break; }
    if (frame->bytes <= track->ring.capacity - track->ring.count) {
      if (track->mic_monitor) {
        /* Transform directly into the protected ring, never a shared scratch frame. */
        const int16_t *input = frame->data;
        for (size_t i = 0u; i < frame->bytes / sizeof(int16_t); ++i) {
          int32_t sample = (int32_t)input[i] * H2_AUDIO_MIC_MONITOR_GAIN;
          if (sample > 32767) sample = 32767;
          if (sample < -32768) sample = -32768;
          int16_t output = (int16_t)sample;
          ring_write(&track->ring, &output, sizeof(output));
        }
      } else {
        ring_write(&track->ring, frame->data, (uint32_t)frame->bytes);
      }
      track->accepted += frame->bytes;
      rc = H2_AUDIO_OK;
      break;
    }
    rc = audio_wait(start, wait_ms);
    if (rc != H2_AUDIO_OK) break;
  }
  --track->operations;
  audio_changed();
  audio_unlock();
  return rc;
}
static int track_drain(h2_pal_audio_track_t *pal_track, uint32_t wait_ms) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  uint32_t start = timer_get_ms();
  int rc = audio_lock(start, wait_ms);
  if (rc != H2_AUDIO_OK) return rc;
  if (track->state != AUDIO_OPEN) { audio_unlock(); return H2_AUDIO_ERR_INVALID_STATE; }
  ++track->operations;
  const uint64_t target = track->accepted;
  /* Like the audio_mixer drain marker, acknowledge prior queued PCM when the
   * downstream consumer has taken it. Future writes do not extend this wait. */
  for (;;) {
    if (track->state != AUDIO_OPEN) { rc = H2_AUDIO_ERR_INVALID_STATE; break; }
    if (track->consumed >= target) { rc = H2_AUDIO_OK; break; }
    rc = audio_wait(start, wait_ms);
    if (rc != H2_AUDIO_OK) break;
  }
  --track->operations;
  audio_changed();
  audio_unlock();
  return rc;
}
static int track_close(h2_pal_audio_track_t *pal_track) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  uintptr_t generation = track->generation;
  while (track->state == AUDIO_OPENING || track->state == AUDIO_CLOSING) {
    if (audio_wait(0u, UINT32_MAX) != H2_AUDIO_OK) { audio_unlock(); return H2_AUDIO_ERR_IO; }
    if (track->generation != generation) { audio_unlock(); return H2_AUDIO_OK; }
  }
  if (track->state == AUDIO_FREE) { audio_unlock(); return H2_AUDIO_OK; }
  track->state = AUDIO_CLOSING;
  audio_changed();
  while (track->operations != 0u) {
    (void)audio_wait(0u, UINT32_MAX);
  }
  struct server *server = track->server;
  audio_unlock();
  union audio_req request = {0};
  request.dec.cmd = AUDIO_DEC_STOP;
  rc = server_request(server, AUDIO_REQ_DEC, &request);
  server_close(server);
  (void)audio_lock(0u, UINT32_MAX);
  free(track->ring.data);
  track->ring = (audio_ring_t){0};
  track->server = NULL;
  track->state = AUDIO_FREE;
  audio_changed();
  audio_unlock();
  return rc == 0 ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}
static int track_get_volume(h2_pal_audio_track_t *pal_track, uint32_t *out_factor_milli) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  *out_factor_milli = track->volume_factor_milli;
  audio_unlock();
  return H2_AUDIO_OK;
}
static int track_set_volume(h2_pal_audio_track_t *pal_track, uint32_t factor_milli) {
  jieli_audio_track_t *track = (jieli_audio_track_t *)pal_track;
  if (factor_milli > 1000u) return H2_AUDIO_ERR_INVALID_ARG;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  if (track->state != AUDIO_OPEN) { audio_unlock(); return H2_AUDIO_ERR_INVALID_STATE; }
  track->volume_factor_milli = factor_milli;
  ++track->operations;
  struct server *server = track->server;
  union audio_req request = {0};
  request.dec.cmd = AUDIO_DEC_SET_VOLUME;
  request.dec.volume = (uint8_t)(audio_state.speaker_volume_percent * factor_milli / 1000u);
  audio_unlock();
  rc = server_request(server, AUDIO_REQ_DEC, &request);
  (void)audio_lock(0u, UINT32_MAX);
  --track->operations;
  audio_changed();
  audio_unlock();
  return rc == 0 ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}
static int audio_create_track(void *user, const h2_audio_track_config_t *config, h2_pal_audio_track_t **out_track) {
  (void)user;
  if (config == NULL || out_track == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  *out_track = NULL;
  if (config->format.sample_rate_hz != H2_AUDIO_SAMPLE_RATE || config->format.channels != 1u ||
      config->format.sample_format != H2_AUDIO_SAMPLE_S16LE || config->volume_factor_milli > 1000u) return H2_AUDIO_ERR_UNSUPPORTED;
  if (config->buffer_frames > UINT32_MAX / H2_AUDIO_FRAME_BYTES) return H2_AUDIO_ERR_INVALID_ARG;
  jieli_audio_track_t *track = NULL;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  if (!audio_state.speaker_started || audio_state.speaker_stopping) { audio_unlock(); return H2_AUDIO_ERR_INVALID_STATE; }
  for (size_t i = 0u; i < H2_AUDIO_MAX_TRACKS; ++i) {
    if (audio_state.tracks[i].state == AUDIO_FREE) { track = &audio_state.tracks[i]; break; }
  }
  if (track == NULL) { audio_unlock(); return H2_PAL_ERR_BUSY; }
  uintptr_t generation = audio_generation();
  if (generation == 0u) { audio_unlock(); return H2_AUDIO_ERR_UNAVAILABLE; }
  memset(track, 0, sizeof(*track));
  track->generation = generation;
  track->state = AUDIO_OPENING;
  track->volume_factor_milli = config->volume_factor_milli;
  track->mic_monitor = config->name != NULL && strcmp(config->name, "audio-system-mic") == 0;
  uint8_t volume = (uint8_t)(audio_state.speaker_volume_percent * config->volume_factor_milli / 1000u);
  track->pal = (h2_pal_audio_track_t){.user = track, .audio = h2_jieli_ac791n_devkit_audio_api(),
      .write = track_write, .close = track_close, .get_volume_factor = track_get_volume,
      .set_volume_factor = track_set_volume, .drain = track_drain};
  uint32_t capacity = (uint32_t)(H2_AUDIO_FRAME_BYTES * (config->buffer_frames != 0u ? config->buffer_frames : H2_AUDIO_TRACK_QUEUE_FRAMES));
  audio_unlock();
  audio_stage("H2_JIELI_AUDIO stage=pcm-open-before\r\n");
  uint8_t *storage = malloc(capacity);
  struct server *server = storage == NULL ? NULL : server_open("audio_server", "dec");
  (void)audio_lock(0u, UINT32_MAX);
  track->ring = (audio_ring_t){.data = storage, .capacity = capacity};
  track->server = server;
  audio_unlock();
  rc = storage == NULL ? H2_AUDIO_ERR_NO_MEMORY : (server == NULL ? H2_AUDIO_ERR_UNAVAILABLE : H2_AUDIO_OK);
  union audio_req request = {0};
  request.dec.cmd = AUDIO_DEC_OPEN;
  request.dec.channel = 2u;
  request.dec.output_buf_len = capacity;
  request.dec.volume = volume;
  request.dec.sample_rate = H2_AUDIO_SAMPLE_RATE;
  request.dec.vfs_ops = &track_vfs_ops;
  request.dec.dec_type = "pcm";
#if defined(CONFIG_AUDIO_DEC_PLAY_SOURCE)
  request.dec.sample_source = CONFIG_AUDIO_DEC_PLAY_SOURCE;
#else
  request.dec.sample_source = "dac";
#endif
#if defined(CONFIG_AUDIO_MIX_ENABLE)
  request.dec.attr = AUDIO_ATTR_REAL_TIME;
#endif
  request.dec.file = (FILE *)generation;
  if (rc == H2_AUDIO_OK && server_request(server, AUDIO_REQ_DEC, &request) != 0) rc = H2_AUDIO_ERR_IO;
  if (rc == H2_AUDIO_OK) {
    audio_stage("H2_JIELI_AUDIO stage=pcm-open-after\r\n");
    audio_stage("H2_JIELI_AUDIO stage=pcm-start-before\r\n");
    request.dec.cmd = AUDIO_DEC_START;
    if (server_request(server, AUDIO_REQ_DEC, &request) != 0) rc = H2_AUDIO_ERR_IO;
  }
  if (rc == H2_AUDIO_OK) audio_stage("H2_JIELI_AUDIO stage=pcm-start-after\r\n");
  if (rc != H2_AUDIO_OK && server != NULL) {
    (void)audio_lock(0u, UINT32_MAX);
    track->state = AUDIO_CLOSING;
    audio_changed();
    audio_unlock();
    request.dec.cmd = AUDIO_DEC_STOP;
    (void)server_request(server, AUDIO_REQ_DEC, &request);
    server_close(server);
  }
  (void)audio_lock(0u, UINT32_MAX);
  if (rc == H2_AUDIO_OK) {
    track->state = AUDIO_OPEN;
    *out_track = &track->pal;
  } else {
    free(storage);
    track->ring = (audio_ring_t){0};
    track->server = NULL;
    track->state = AUDIO_FREE;
  }
  audio_changed();
  audio_unlock();
  return rc;
}
static int audio_stop_speaker(void *user) {
  (void)user;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  while (audio_state.speaker_stopping) {
    if (audio_wait(0u, UINT32_MAX) != H2_AUDIO_OK) { audio_unlock(); return H2_AUDIO_ERR_IO; }
  }
  audio_state.speaker_stopping = 1;
  audio_state.speaker_started = 0;
  audio_unlock();
  int result = H2_AUDIO_OK;
  for (size_t i = 0u; i < H2_AUDIO_MAX_TRACKS; ++i) {
    rc = track_close(&audio_state.tracks[i].pal);
    if (result == H2_AUDIO_OK) result = rc;
  }
  (void)audio_lock(0u, UINT32_MAX);
  audio_state.speaker_stopping = 0;
  audio_changed();
  audio_unlock();
  return result;
}
static int audio_get_volume(void *user, uint32_t *out_percent) {
  (void)user;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  *out_percent = audio_state.speaker_volume_percent;
  audio_unlock();
  return H2_AUDIO_OK;
}
static int audio_set_volume(void *user, uint32_t percent) {
  (void)user;
  if (percent > 100u) return H2_AUDIO_ERR_INVALID_ARG;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  audio_state.speaker_volume_percent = percent;
  struct server *servers[H2_AUDIO_MAX_TRACKS] = {0};
  uint8_t volumes[H2_AUDIO_MAX_TRACKS] = {0};
  for (size_t i = 0u; i < H2_AUDIO_MAX_TRACKS; ++i) {
    jieli_audio_track_t *track = &audio_state.tracks[i];
    if (track->state == AUDIO_OPEN) {
      ++track->operations;
      servers[i] = track->server;
      volumes[i] = (uint8_t)(percent * track->volume_factor_milli / 1000u);
    }
  }
  audio_unlock();
  int result = H2_AUDIO_OK;
  for (size_t i = 0u; i < H2_AUDIO_MAX_TRACKS; ++i) {
    if (servers[i] == NULL) continue;
    union audio_req request = {0};
    request.dec.cmd = AUDIO_DEC_SET_VOLUME;
    request.dec.volume = volumes[i];
    if (server_request(servers[i], AUDIO_REQ_DEC, &request) != 0) result = H2_AUDIO_ERR_IO;
    (void)audio_lock(0u, UINT32_MAX);
    --audio_state.tracks[i].operations;
    audio_changed();
    audio_unlock();
  }
  return result;
}

int h2_jieli_ac791n_devkit_audio_idle_probe(h2_jieli_ac791n_devkit_audio_idle_t *out) {
  if (out == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  int rc = audio_lock(0u, UINT32_MAX);
  if (rc != H2_AUDIO_OK) return rc;
  *out = (h2_jieli_ac791n_devkit_audio_idle_t){
      .sdk_servers = audio_state.mic_server != NULL,
      .mic_open = audio_state.mic_state != AUDIO_FREE,
      .speaker_started = audio_state.speaker_started != 0,
  };
  for (size_t i = 0u; i < H2_AUDIO_MAX_TRACKS; ++i) {
    const jieli_audio_track_t *track = &audio_state.tracks[i];
    out->open_tracks += track->state != AUDIO_FREE;
    out->retained_operations += track->operations;
    out->ring_bytes += track->ring.capacity;
    out->sdk_servers += track->server != NULL;
    if (track->state == AUDIO_OPEN) out->consumed_bytes += track->consumed;
  }
  audio_unlock();
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

int h2_jieli_ac791n_devkit_audio_idle_probe(h2_jieli_ac791n_devkit_audio_idle_t *out) {
  (void)out;
  return H2_AUDIO_ERR_UNSUPPORTED;
}

const h2_pal_audio_api_t *h2_jieli_ac791n_devkit_audio_api(void) {
  return h2_pal_unsupported_audio_api();
}

#endif
