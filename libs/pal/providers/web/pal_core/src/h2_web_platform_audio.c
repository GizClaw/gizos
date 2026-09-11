#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>

// Playback queue depth advertised by get_info and enforced by track write.
#define H2_WEB_AUDIO_TRACK_QUEUE_FRAMES 8u
#define H2_WEB_AUDIO_MIN_QUEUE_MS 200u

struct h2_web_audio_track {
  h2_pal_audio_track_t base;
  h2_web_audio_track_t *next;
  bool suspended_reported;
  h2_web_platform_t *platform;
  h2_audio_pcm_format_t format;
  uint32_t volume_factor_milli;
};

EM_JS(void, h2_web_audio_init_js, (uintptr_t platform_address), {
  const platforms = Module['h2WebAudioPlatforms'] ||= new Map();
  if (platforms.has(platform_address)) return;
  const state = {
    context: Module['h2WebAudioContext'] || null,
    tracks: new Map(),
    activate: null,
    speakerStarted: false,
  };
  const ensureContext = () => {
    const AudioContext = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!AudioContext) return null;
    state.context ||= new AudioContext();
    Module['h2WebAudioContext'] ||= state.context;
    if (state.context.state === 'suspended') {
      state.context.resume().catch(() => {});
    }
    return state.context;
  };
  state.activate = ensureContext;
  if (typeof globalThis.addEventListener === 'function') {
    globalThis.addEventListener('pointerdown', ensureContext, {passive: true});
    globalThis.addEventListener('keydown', ensureContext, {passive: true});
  }
  platforms.set(platform_address, state);
});

EM_JS(void, h2_web_audio_deinit_js, (uintptr_t platform_address), {
  const platforms = Module['h2WebAudioPlatforms'];
  const state = platforms && platforms.get(platform_address);
  if (!state) return;
  if (typeof globalThis.removeEventListener === 'function') {
    globalThis.removeEventListener('pointerdown', state.activate);
    globalThis.removeEventListener('keydown', state.activate);
  }
  for (const track of state.tracks.values()) {
    for (const source of track.sources) {
      try { source.stop(); } catch (_) {}
    }
  }
  if (state.context) {
    if (Module['h2WebAudioContext'] === state.context)
      Module['h2WebAudioContext'] = null;
    state.context.close().catch(() => {});
  }
  platforms.delete(platform_address);
});

EM_JS(int, h2_web_audio_start_js, (uintptr_t platform_address), {
  const state = Module['h2WebAudioPlatforms']?.get(platform_address);
  if (!state || !state.activate()) return -3;
  state.speakerStarted = true;
  return 0;
});

EM_JS(void, h2_web_audio_stop_js, (uintptr_t platform_address), {
  const state = Module['h2WebAudioPlatforms']?.get(platform_address);
  if (!state) return;
  state.speakerStarted = false;
  for (const track of state.tracks.values()) {
    for (const source of track.sources) {
      try { source.stop(); } catch (_) {}
    }
    track.sources.clear();
    track.nextTime = 0;
  }
});

EM_JS(void, h2_web_audio_track_open_js,
      (uintptr_t platform_address, uintptr_t track_address, double gain), {
        const state = Module['h2WebAudioPlatforms']?.get(platform_address);
        if (state) {
          state.tracks.set(track_address,
                           {nextTime: 0, sources: new Set(), gain, node: null});
        }
      });

// A persistent GainNode per track applies volume changes to audio that is
// already scheduled instead of only to later writes.
EM_JS(void, h2_web_audio_track_gain_js,
      (uintptr_t platform_address, uintptr_t track_address, double gain), {
        const track = Module['h2WebAudioPlatforms']?.get(platform_address)
            ?.tracks.get(track_address);
        if (!track) return;
        track.gain = Math.max(0, Math.min(1, gain));
        if (track.node) track.node.gain.value = track.gain;
      });

// Seconds of audio scheduled but not yet played; -1 when the track is gone.
// out_suspended reports a context that is not running (autoplay policy).
EM_JS(double, h2_web_audio_track_queued_js,
      (uintptr_t platform_address, uintptr_t track_address,
       int *out_suspended), {
        const state = Module['h2WebAudioPlatforms']?.get(platform_address);
        const track = state?.tracks.get(track_address);
        HEAP32[out_suspended >>> 2] = 0;
        if (!track) return -1;
        const context = state.context;
        if (!context) return 0;
        HEAP32[out_suspended >>> 2] = context.state === 'running' ? 0 : 1;
        // Output latency counts too: "drained" means heard, not handed off.
        const latency = (context.outputLatency || 0) + (context.baseLatency || 0);
        return track.nextTime > context.currentTime
            ? track.nextTime - context.currentTime + latency
            : 0;
      });

EM_JS(int, h2_web_audio_track_write_js,
      (uintptr_t platform_address, uintptr_t track_address,
       const int16_t *samples, uint32_t samples_per_channel, int channels,
       uint32_t sample_rate_hz), {
        const state = Module['h2WebAudioPlatforms']?.get(platform_address);
        const track = state?.tracks.get(track_address);
        const context = state?.activate();
        if (!track || !context) return -7;
        try {
          const buffer = context.createBuffer(channels, samples_per_channel,
                                              sample_rate_hz);
          // DataView tolerates PCM that is not 2-byte aligned.
          const view = new DataView(HEAPU8.buffer, samples,
                                    samples_per_channel * channels * 2);
          for (let channel = 0; channel < channels; ++channel) {
            const output = buffer.getChannelData(channel);
            for (let index = 0; index < samples_per_channel; ++index) {
              output[index] =
                  view.getInt16((index * channels + channel) * 2, true) /
                  32768.0;
            }
          }
          if (!track.node || track.node.context !== context) {
            track.node = context.createGain();
            track.node.gain.value = track.gain;
            track.node.connect(context.destination);
          }
          const source = context.createBufferSource();
          source.buffer = buffer;
          source.connect(track.node);
          const startTime = Math.max(context.currentTime, track.nextTime);
          track.sources.add(source);
          source.onended = () => track.sources.delete(source);
          source.start(startTime);
          track.nextTime = startTime + buffer.duration;
          return 0;
        } catch (error) {
          console.error('Web Audio playback failed', error);
          return -4;
        }
      });

EM_JS(void, h2_web_audio_track_close_js,
      (uintptr_t platform_address, uintptr_t track_address), {
        const state = Module['h2WebAudioPlatforms']?.get(platform_address);
        const track = state?.tracks.get(track_address);
        if (!track) return;
        for (const source of track.sources) {
          try { source.stop(); } catch (_) {}
        }
        try { track.node?.disconnect(); } catch (_) {}
        state.tracks.delete(track_address);
      });

static double h2_web_audio_track_gain(const h2_web_audio_track_t *track) {
  return (double)track->volume_factor_milli *
         (double)track->platform->speaker_volume_percent / 100000.0;
}

static uint32_t h2_web_audio_queue_capacity_ms(const h2_web_audio_track_t *track) {
  const uint64_t frame_ms =
      (uint64_t)track->format.frame_samples_per_channel * 1000u /
      track->format.sample_rate_hz;
  const uint64_t capacity = frame_ms * H2_WEB_AUDIO_TRACK_QUEUE_FRAMES;
  return capacity < H2_WEB_AUDIO_MIN_QUEUE_MS ? H2_WEB_AUDIO_MIN_QUEUE_MS
                                              : (uint32_t)capacity;
}

/*
 * Wait until at most target_ms of audio remains scheduled. The Web Audio
 * clock provides no completion event, so waits sleep until the expected
 * playback time. Returns WOULD_BLOCK when timeout_ms passes first, including
 * while an autoplay policy keeps the AudioContext suspended.
 */
static int h2_web_audio_track_wait(h2_web_audio_track_t *track,
                                   double target_ms, uint32_t timeout_ms) {
  const double deadline_ms = emscripten_get_now() + (double)timeout_ms;
  for (;;) {
    int suspended = 0;
    const double queued_s = h2_web_audio_track_queued_js(
        (uintptr_t)track->platform, (uintptr_t)track, &suspended);
    if (queued_s < 0.0)
      return H2_AUDIO_ERR_INVALID_STATE;
    const double excess_ms = queued_s * 1000.0 - target_ms;
    if (excess_ms <= 0.0)
      return H2_AUDIO_OK;
    if (suspended && !track->suspended_reported) {
      track->suspended_reported = true;
      (void)h2_pal_log_write(
          h2_web_platform_log_api(), H2_PAL_LOG_WARN, "web_audio",
          "AudioContext is suspended; playback waits for a user gesture");
    }
    const double now_ms = emscripten_get_now();
    if (timeout_ms != UINT32_MAX && now_ms >= deadline_ms)
      return H2_AUDIO_ERR_WOULD_BLOCK;
    double sleep_ms = excess_ms < 2.0 ? 2.0 : excess_ms;
    if (sleep_ms > 50.0)
      sleep_ms = 50.0;
    if (timeout_ms != UINT32_MAX && now_ms + sleep_ms > deadline_ms)
      sleep_ms = deadline_ms - now_ms;
    const int result =
        h2_web_platform_sleep_ms(track->platform, (uint32_t)sleep_ms + 1u);
    if (result != H2_PAL_OK)
      return result == H2_PAL_EXIT ? H2_PAL_ERR_CLOSED : result;
  }
}

static int h2_web_audio_get_info(void *user, h2_audio_info_t *out_info) {
  if (user == NULL || out_info == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  *out_info = (h2_audio_info_t){
      .available = 1,
      .mic_supported = h2_web_platform_mic_supported(),
      .mic_format = {16000u, 320u, 1u, H2_AUDIO_SAMPLE_S16LE},
      .mic_queue_frames = 8u,
      .playback_supported = 1,
      .playback_format =
          {
              .sample_rate_hz = 16000u,
              .frame_samples_per_channel = 960u,
              .channels = 1u,
              .sample_format = H2_AUDIO_SAMPLE_S16LE,
          },
      .track_queue_frames = H2_WEB_AUDIO_TRACK_QUEUE_FRAMES,
      .max_tracks = 4u,
  };
  return H2_AUDIO_OK;
}

static int h2_web_audio_start_speaker(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  const int result = h2_web_audio_start_js((uintptr_t)platform);
  if (result == H2_AUDIO_OK) {
    platform->speaker_started = true;
    platform->speaker_stopped = false;
  }
  return result;
}

static int h2_web_audio_stop_speaker(void *user) {
  h2_web_platform_t *platform = user;
  if (platform == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  platform->speaker_started = false;
  platform->speaker_stopped = true;
  h2_web_audio_stop_js((uintptr_t)platform);
  return H2_AUDIO_OK;
}

static int h2_web_audio_track_write(h2_pal_audio_track_t *base,
                                    const h2_audio_frame_t *frame,
                                    uint32_t timeout_ms) {
  h2_web_audio_track_t *track = (h2_web_audio_track_t *)base;
  if (track == NULL || frame == NULL || frame->data == NULL ||
      frame->sample_rate_hz != track->format.sample_rate_hz ||
      frame->channels != track->format.channels ||
      frame->sample_format != track->format.sample_format ||
      frame->samples_per_channel == 0u ||
      frame->bytes != (size_t)frame->samples_per_channel * frame->channels *
                          sizeof(int16_t)) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  if (track->platform->speaker_stopped) {
    return H2_AUDIO_ERR_INVALID_STATE;
  }
  // Keep at most track_queue_frames of audio ahead of the playback clock.
  const double frame_ms = (double)frame->samples_per_channel * 1000.0 /
                          (double)frame->sample_rate_hz;
  double target_ms = (double)h2_web_audio_queue_capacity_ms(track) - frame_ms;
  if (target_ms < 0.0)
    target_ms = 0.0;
  const int wait = h2_web_audio_track_wait(track, target_ms, timeout_ms);
  if (wait != H2_AUDIO_OK)
    return wait;
  if (track->platform->speaker_stopped)
    return H2_AUDIO_ERR_INVALID_STATE;
  return h2_web_audio_track_write_js(
      (uintptr_t)track->platform, (uintptr_t)track, frame->data,
      frame->samples_per_channel, frame->channels, frame->sample_rate_hz);
}

static int h2_web_audio_track_close(h2_pal_audio_track_t *base) {
  h2_web_audio_track_t *track = (h2_web_audio_track_t *)base;
  if (track == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  h2_web_audio_track_close_js((uintptr_t)track->platform, (uintptr_t)track);
  h2_web_audio_track_t **cursor = &track->platform->audio_tracks;
  while (*cursor != NULL && *cursor != track)
    cursor = &(*cursor)->next;
  if (*cursor == track)
    *cursor = track->next;
  free(track);
  return H2_AUDIO_OK;
}

static int h2_web_audio_track_get_volume(h2_pal_audio_track_t *base,
                                         uint32_t *out_factor_milli) {
  h2_web_audio_track_t *track = (h2_web_audio_track_t *)base;
  if (track == NULL || out_factor_milli == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  *out_factor_milli = track->volume_factor_milli;
  return H2_AUDIO_OK;
}

static int h2_web_audio_track_set_volume(h2_pal_audio_track_t *base,
                                         uint32_t factor_milli) {
  h2_web_audio_track_t *track = (h2_web_audio_track_t *)base;
  if (track == NULL || factor_milli > 1000u) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  track->volume_factor_milli = factor_milli;
  h2_web_audio_track_gain_js((uintptr_t)track->platform, (uintptr_t)track,
                             h2_web_audio_track_gain(track));
  return H2_AUDIO_OK;
}

/* Wait until every scheduled sample has played; WOULD_BLOCK on timeout. */
static int h2_web_audio_track_drain(h2_pal_audio_track_t *base,
                                    uint32_t timeout_ms) {
  h2_web_audio_track_t *track = (h2_web_audio_track_t *)base;
  if (track == NULL)
    return H2_AUDIO_ERR_INVALID_ARG;
  if (track->platform->speaker_stopped)
    return H2_AUDIO_OK;
  return h2_web_audio_track_wait(track, 0.0, timeout_ms);
}

static int h2_web_audio_create_track(void *user,
                                     const h2_audio_track_config_t *config,
                                     h2_pal_audio_track_t **out_track) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || config == NULL || out_track == NULL ||
      config->format.sample_rate_hz == 0u ||
      config->format.frame_samples_per_channel == 0u ||
      config->format.channels == 0u || config->format.channels > 2u ||
      config->format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      config->volume_factor_milli > 1000u) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  h2_web_audio_track_t *track = calloc(1u, sizeof(*track));
  if (track == NULL) {
    return H2_AUDIO_ERR_NO_MEMORY;
  }
  track->base = (h2_pal_audio_track_t){
      .user = track,
      .audio = &platform->audio_api,
      .write = h2_web_audio_track_write,
      .close = h2_web_audio_track_close,
      .get_volume_factor = h2_web_audio_track_get_volume,
      .set_volume_factor = h2_web_audio_track_set_volume,
      .drain = h2_web_audio_track_drain,
  };
  track->platform = platform;
  track->format = config->format;
  track->volume_factor_milli = config->volume_factor_milli;
  h2_web_audio_track_open_js((uintptr_t)platform, (uintptr_t)track,
                             h2_web_audio_track_gain(track));
  track->next = platform->audio_tracks;
  platform->audio_tracks = track;
  *out_track = &track->base;
  return H2_AUDIO_OK;
}

static int h2_web_audio_get_speaker_volume(void *user,
                                           uint32_t *out_percent) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || out_percent == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  *out_percent = platform->speaker_volume_percent;
  return H2_AUDIO_OK;
}

static int h2_web_audio_set_speaker_volume(void *user, uint32_t percent) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || percent > 100u) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  platform->speaker_volume_percent = percent;
  for (h2_web_audio_track_t *track = platform->audio_tracks; track != NULL;
       track = track->next) {
    h2_web_audio_track_gain_js((uintptr_t)platform, (uintptr_t)track,
                               h2_web_audio_track_gain(track));
  }
  return H2_AUDIO_OK;
}

static const h2_pal_audio_vtable_t h2_web_audio_vtable = {
    .get_info = h2_web_audio_get_info,
    .start_mic = h2_web_platform_mic_start,
    .stop_mic = h2_web_platform_mic_stop,
    .start_speaker = h2_web_audio_start_speaker,
    .stop_speaker = h2_web_audio_stop_speaker,
    .mic_read = h2_web_platform_mic_read,
    .create_track = h2_web_audio_create_track,
    .get_speaker_volume_percent = h2_web_audio_get_speaker_volume,
    .set_speaker_volume_percent = h2_web_audio_set_speaker_volume,
};

void h2_web_platform_audio_init(h2_web_platform_t *platform) {
  platform->audio_api = (h2_pal_audio_api_t){
      .user = platform,
      .vtable = &h2_web_audio_vtable,
  };
  platform->speaker_volume_percent = 100u;
  h2_web_audio_init_js((uintptr_t)platform);
}

void h2_web_platform_audio_deinit(h2_web_platform_t *platform) {
  (void)h2_web_platform_mic_stop(platform);
  h2_web_audio_deinit_js((uintptr_t)platform);
}
