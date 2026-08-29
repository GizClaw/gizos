#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct h2_web_audio_track {
  h2_pal_audio_track_t base;
  h2_web_platform_t *platform;
  h2_audio_pcm_format_t format;
  uint32_t volume_factor_milli;
} h2_web_audio_track_t;

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
      (uintptr_t platform_address, uintptr_t track_address), {
        const state = Module['h2WebAudioPlatforms']?.get(platform_address);
        if (state) {
          state.tracks.set(track_address, {nextTime: 0, sources: new Set()});
        }
      });

EM_JS(int, h2_web_audio_track_write_js,
      (uintptr_t platform_address, uintptr_t track_address,
       const int16_t *samples, uint32_t samples_per_channel, int channels,
       uint32_t sample_rate_hz, double gain), {
        const state = Module['h2WebAudioPlatforms']?.get(platform_address);
        const track = state?.tracks.get(track_address);
        const context = state?.activate();
        if (!track || !context) return -7;
        try {
          const buffer = context.createBuffer(channels, samples_per_channel,
                                              sample_rate_hz);
          const sampleOffset = samples >> 1;
          for (let channel = 0; channel < channels; ++channel) {
            const output = buffer.getChannelData(channel);
            for (let index = 0; index < samples_per_channel; ++index) {
              output[index] = HEAP16[sampleOffset + index * channels + channel] /
                              32768.0;
            }
          }
          const source = context.createBufferSource();
          const gainNode = context.createGain();
          gainNode.gain.value = Math.max(0, Math.min(1, gain));
          source.buffer = buffer;
          source.connect(gainNode).connect(context.destination);
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
        state.tracks.delete(track_address);
      });

static int h2_web_audio_get_info(void *user, h2_audio_info_t *out_info) {
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
              .frame_samples_per_channel = 960u,
              .channels = 1u,
              .sample_format = H2_AUDIO_SAMPLE_S16LE,
          },
      .track_queue_frames = 8u,
      .max_tracks = 4u,
  };
  return H2_AUDIO_OK;
}

static int h2_web_audio_unsupported(void *user) {
  return user == NULL ? H2_AUDIO_ERR_INVALID_ARG : H2_AUDIO_ERR_UNSUPPORTED;
}

static int h2_web_audio_mic_read(void *user, h2_audio_frame_t *out_frame,
                                 uint32_t timeout_ms) {
  (void)out_frame;
  (void)timeout_ms;
  return user == NULL ? H2_AUDIO_ERR_INVALID_ARG : H2_AUDIO_ERR_UNSUPPORTED;
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
  (void)timeout_ms;
  h2_web_audio_track_t *track = (h2_web_audio_track_t *)base;
  if (track == NULL || frame == NULL || frame->data == NULL ||
      track->platform->speaker_stopped ||
      frame->sample_rate_hz != track->format.sample_rate_hz ||
      frame->channels != track->format.channels ||
      frame->sample_format != track->format.sample_format ||
      frame->samples_per_channel == 0u ||
      frame->bytes != (size_t)frame->samples_per_channel * frame->channels *
                          sizeof(int16_t)) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  const double gain =
      (double)track->volume_factor_milli *
      (double)track->platform->speaker_volume_percent / 100000.0;
  return h2_web_audio_track_write_js(
      (uintptr_t)track->platform, (uintptr_t)track, frame->data,
      frame->samples_per_channel, frame->channels, frame->sample_rate_hz, gain);
}

static int h2_web_audio_track_close(h2_pal_audio_track_t *base) {
  h2_web_audio_track_t *track = (h2_web_audio_track_t *)base;
  if (track == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  h2_web_audio_track_close_js((uintptr_t)track->platform, (uintptr_t)track);
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
  return H2_AUDIO_OK;
}

static int h2_web_audio_track_drain(h2_pal_audio_track_t *base,
                                    uint32_t timeout_ms) {
  (void)timeout_ms;
  return base == NULL ? H2_AUDIO_ERR_INVALID_ARG : H2_AUDIO_OK;
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
  h2_web_audio_track_open_js((uintptr_t)platform, (uintptr_t)track);
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
  return H2_AUDIO_OK;
}

static const h2_pal_audio_vtable_t h2_web_audio_vtable = {
    .get_info = h2_web_audio_get_info,
    .start_mic = h2_web_audio_unsupported,
    .stop_mic = h2_web_audio_unsupported,
    .start_speaker = h2_web_audio_start_speaker,
    .stop_speaker = h2_web_audio_stop_speaker,
    .mic_read = h2_web_audio_mic_read,
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
  h2_web_audio_deinit_js((uintptr_t)platform);
}
