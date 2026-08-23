#define _POSIX_C_SOURCE 200809L

#include "h2_linux_alsa_audio.h"

#include "h2_linux_alsa_abi.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct linux_alsa_state linux_alsa_state_t;

typedef struct linux_alsa_track {
    h2_pal_audio_track_t api;
    linux_alsa_state_t *owner;
    snd_pcm_t *pcm;
    h2_audio_pcm_format_t format;
    uint32_t volume_factor_milli;
    int16_t *scratch;
    size_t scratch_values;
    pthread_mutex_t mutex;
} linux_alsa_track_t;

struct linux_alsa_state {
    pthread_mutex_t mutex;
    h2_linux_alsa_audio_config_t config;
    char device[64];
    uint32_t speaker_volume_percent;
    int configured;
    int speaker_started;
    h2_pal_audio_track_t *track;
};

static linux_alsa_state_t s_audio = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .speaker_volume_percent = 100u,
};
static pthread_once_t s_symbols_once = PTHREAD_ONCE_INIT;
static h2_linux_alsa_symbols_t s_symbols;
static int s_symbols_result = H2_AUDIO_ERR_UNAVAILABLE;

#if !defined(H2_LINUX_ALSA_TESTING)
static void *s_alsa_library;

static void copy_symbol(void *destination, void *symbol) {
    memcpy(destination, &symbol, sizeof(symbol));
}
#endif

static void load_symbols_once(void) {
#if defined(H2_LINUX_ALSA_TESTING)
    s_symbols_result = h2_linux_alsa_test_load_symbols(&s_symbols);
#else
    s_alsa_library = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (s_alsa_library == NULL) {
        return;
    }
#define H2_LOAD_ALSA(field, name)                                      \
    do {                                                               \
        void *symbol = dlsym(s_alsa_library, name);                     \
        if (symbol == NULL) return;                                     \
        copy_symbol(&s_symbols.field, symbol);                          \
    } while (0)
    H2_LOAD_ALSA(pcm_open, "snd_pcm_open");
    H2_LOAD_ALSA(pcm_set_params, "snd_pcm_set_params");
    H2_LOAD_ALSA(pcm_writei, "snd_pcm_writei");
    H2_LOAD_ALSA(pcm_wait, "snd_pcm_wait");
    H2_LOAD_ALSA(pcm_recover, "snd_pcm_recover");
    H2_LOAD_ALSA(pcm_drop, "snd_pcm_drop");
    H2_LOAD_ALSA(pcm_close, "snd_pcm_close");
#undef H2_LOAD_ALSA
    s_symbols_result = H2_AUDIO_OK;
#endif
}

static int load_symbols(void) {
    return pthread_once(&s_symbols_once, load_symbols_once) == 0
        ? s_symbols_result : H2_AUDIO_ERR_UNAVAILABLE;
}

static int pcm_format_valid(const h2_audio_pcm_format_t *format) {
    return format != NULL && format->sample_rate_hz != 0u &&
        format->frame_samples_per_channel != 0u &&
        (format->channels == 1u || format->channels == 2u) &&
        format->sample_format == H2_AUDIO_SAMPLE_S16LE;
}

static int pcm_format_equal(
    const h2_audio_pcm_format_t *left,
    const h2_audio_pcm_format_t *right) {
    return left->sample_rate_hz == right->sample_rate_hz &&
        left->frame_samples_per_channel == right->frame_samples_per_channel &&
        left->channels == right->channels &&
        left->sample_format == right->sample_format;
}

h2_pal_result_t h2_linux_alsa_audio_configure(
    const h2_linux_alsa_audio_config_t *config) {
    if (config == NULL || config->device == NULL || config->device[0] == '\0' ||
        strnlen(config->device, sizeof(s_audio.device)) >= sizeof(s_audio.device) ||
        !pcm_format_valid(&config->playback_format)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_audio.mutex);
    if (s_audio.speaker_started || s_audio.track != NULL) {
        (void)pthread_mutex_unlock(&s_audio.mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_audio.config = *config;
    (void)strcpy(s_audio.device, config->device);
    s_audio.config.device = s_audio.device;
    s_audio.configured = 1;
    (void)pthread_mutex_unlock(&s_audio.mutex);
    return H2_PAL_OK;
}

static int audio_get_info(void *user, h2_audio_info_t *info) {
    linux_alsa_state_t *audio = user;
    (void)pthread_mutex_lock(&audio->mutex);
    if (!audio->configured) {
        (void)pthread_mutex_unlock(&audio->mutex);
        return H2_AUDIO_ERR_UNAVAILABLE;
    }
    *info = (h2_audio_info_t){
        .available = 1,
        .playback_supported = 1,
        .playback_format = audio->config.playback_format,
        .track_queue_frames = 1u,
        .max_tracks = 1u,
    };
    (void)pthread_mutex_unlock(&audio->mutex);
    return H2_AUDIO_OK;
}

static int audio_start_mic(void *user) {
    (void)user;
    return H2_AUDIO_ERR_UNSUPPORTED;
}

static int audio_stop_mic(void *user) {
    (void)user;
    return H2_AUDIO_ERR_UNSUPPORTED;
}

static int audio_start_speaker(void *user) {
    linux_alsa_state_t *audio = user;
    (void)pthread_mutex_lock(&audio->mutex);
    if (!audio->configured) {
        (void)pthread_mutex_unlock(&audio->mutex);
        return H2_AUDIO_ERR_UNAVAILABLE;
    }
    audio->speaker_started = 1;
    (void)pthread_mutex_unlock(&audio->mutex);
    return H2_AUDIO_OK;
}

static int audio_stop_speaker(void *user) {
    linux_alsa_state_t *audio = user;
    (void)pthread_mutex_lock(&audio->mutex);
    if (audio->track != NULL) {
        (void)pthread_mutex_unlock(&audio->mutex);
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    audio->speaker_started = 0;
    (void)pthread_mutex_unlock(&audio->mutex);
    return H2_AUDIO_OK;
}

static int audio_mic_read(
    void *user,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms) {
    (void)user;
    (void)out_frame;
    (void)timeout_ms;
    return H2_AUDIO_ERR_UNSUPPORTED;
}

static int monotonic_ms(uint64_t *out_ms) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return H2_AUDIO_ERR_IO;
    }
    *out_ms = (uint64_t)now.tv_sec * UINT64_C(1000) +
        (uint64_t)now.tv_nsec / UINT64_C(1000000);
    return H2_AUDIO_OK;
}

static int remaining_timeout(uint64_t deadline_ms, int *out_timeout_ms) {
    uint64_t now_ms = 0u;
    int result = monotonic_ms(&now_ms);
    if (result != H2_AUDIO_OK) return result;
    if (now_ms >= deadline_ms) return H2_PAL_ERR_TIMEOUT;
    uint64_t remaining = deadline_ms - now_ms;
    *out_timeout_ms = remaining > (uint64_t)INT_MAX
        ? INT_MAX : (int)remaining;
    return H2_AUDIO_OK;
}

static int16_t scale_sample(
    int16_t sample,
    uint32_t track_factor_milli,
    uint32_t speaker_percent) {
    int64_t scaled = (int64_t)sample * track_factor_milli * speaker_percent;
    scaled /= INT64_C(100000);
    if (scaled > INT16_MAX) return INT16_MAX;
    if (scaled < INT16_MIN) return INT16_MIN;
    return (int16_t)scaled;
}

static int track_write(
    h2_pal_audio_track_t *opaque,
    const h2_audio_frame_t *frame,
    uint32_t timeout_ms) {
    linux_alsa_track_t *track = opaque->user;
    const size_t frame_bytes = h2_audio_frame_frame_bytes(frame);
    if (frame_bytes == 0u || frame->bytes > frame->capacity ||
        frame->bytes % frame_bytes != 0u ||
        frame->sample_rate_hz != track->format.sample_rate_hz ||
        frame->samples_per_channel != track->format.frame_samples_per_channel ||
        frame->channels != track->format.channels ||
        frame->sample_format != track->format.sample_format) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    const size_t frames = frame->bytes / frame_bytes;
    if (frames > (size_t)LONG_MAX ||
        frame->bytes / sizeof(int16_t) > track->scratch_values) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&track->mutex);
    uint32_t speaker_percent = 0u;
    (void)pthread_mutex_lock(&track->owner->mutex);
    speaker_percent = track->owner->speaker_volume_percent;
    (void)pthread_mutex_unlock(&track->owner->mutex);
    const int16_t *source = frame->data;
    if (track->volume_factor_milli != 1000u || speaker_percent != 100u) {
        const size_t values = frame->bytes / sizeof(int16_t);
        for (size_t i = 0u; i < values; ++i) {
            track->scratch[i] = scale_sample(
                source[i], track->volume_factor_milli, speaker_percent);
        }
        source = track->scratch;
    }
    uint64_t deadline_ms = UINT64_MAX;
    int result = H2_AUDIO_OK;
    if (timeout_ms != UINT32_MAX) {
        uint64_t now_ms = 0u;
        result = monotonic_ms(&now_ms);
        deadline_ms = UINT64_MAX - now_ms < timeout_ms
            ? UINT64_MAX : now_ms + timeout_ms;
    }
    size_t written = 0u;
    while (result == H2_AUDIO_OK && written < frames) {
        const snd_pcm_sframes_t count = s_symbols.pcm_writei(
            track->pcm,
            source + written * track->format.channels,
            (snd_pcm_uframes_t)(frames - written));
        if (count > 0) {
            written += (size_t)count;
            continue;
        }
        if (count < 0 && count != -EAGAIN) {
            if (count >= INT_MIN &&
                s_symbols.pcm_recover(track->pcm, (int)count, 1) >= 0) {
                continue;
            }
            result = H2_AUDIO_ERR_IO;
            break;
        }
        int wait_ms = -1;
        if (deadline_ms != UINT64_MAX) {
            result = remaining_timeout(deadline_ms, &wait_ms);
            if (result != H2_AUDIO_OK) break;
        }
        const int wait_result = s_symbols.pcm_wait(track->pcm, wait_ms);
        if (wait_result > 0) {
            continue;
        }
        if (wait_result == 0) {
            result = H2_PAL_ERR_TIMEOUT;
        } else if (s_symbols.pcm_recover(track->pcm, wait_result, 1) < 0) {
            result = H2_AUDIO_ERR_IO;
        }
    }
    (void)pthread_mutex_unlock(&track->mutex);
    return result;
}

static int track_close(h2_pal_audio_track_t *opaque) {
    linux_alsa_track_t *track = opaque->user;
    linux_alsa_state_t *owner = track->owner;
    (void)pthread_mutex_lock(&track->mutex);
    int result = s_symbols.pcm_drop(track->pcm) < 0
        ? H2_AUDIO_ERR_IO : H2_AUDIO_OK;
    if (s_symbols.pcm_close(track->pcm) < 0) result = H2_AUDIO_ERR_IO;
    track->pcm = NULL;
    (void)pthread_mutex_unlock(&track->mutex);
    (void)pthread_mutex_destroy(&track->mutex);
    free(track->scratch);
    (void)pthread_mutex_lock(&owner->mutex);
    if (owner->track == opaque) owner->track = NULL;
    (void)pthread_mutex_unlock(&owner->mutex);
    free(track);
    return result;
}

static int track_get_volume(
    h2_pal_audio_track_t *opaque,
    uint32_t *out_factor_milli) {
    linux_alsa_track_t *track = opaque->user;
    (void)pthread_mutex_lock(&track->mutex);
    *out_factor_milli = track->volume_factor_milli;
    (void)pthread_mutex_unlock(&track->mutex);
    return H2_AUDIO_OK;
}

static int track_set_volume(
    h2_pal_audio_track_t *opaque,
    uint32_t factor_milli) {
    linux_alsa_track_t *track = opaque->user;
    if (factor_milli > 4000u) return H2_AUDIO_ERR_INVALID_ARG;
    (void)pthread_mutex_lock(&track->mutex);
    track->volume_factor_milli = factor_milli;
    (void)pthread_mutex_unlock(&track->mutex);
    return H2_AUDIO_OK;
}

static int track_drain(h2_pal_audio_track_t *opaque, uint32_t timeout_ms) {
    (void)opaque;
    (void)timeout_ms;
    return H2_AUDIO_ERR_UNSUPPORTED;
}

static int audio_create_track(
    void *user,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
    linux_alsa_state_t *audio = user;
    if (!pcm_format_valid(&config->format) ||
        config->volume_factor_milli > 4000u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&audio->mutex);
    if (!audio->configured || !audio->speaker_started || audio->track != NULL ||
        !pcm_format_equal(&config->format, &audio->config.playback_format)) {
        (void)pthread_mutex_unlock(&audio->mutex);
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    const char *device = audio->config.device;
    (void)pthread_mutex_unlock(&audio->mutex);
    int result = load_symbols();
    if (result != H2_AUDIO_OK) return result;
    linux_alsa_track_t *track = calloc(1u, sizeof(*track));
    if (track == NULL) return H2_AUDIO_ERR_NO_MEMORY;
    const size_t values =
        (size_t)config->format.frame_samples_per_channel * config->format.channels;
    track->scratch = calloc(values, sizeof(int16_t));
    if (track->scratch == NULL || pthread_mutex_init(&track->mutex, NULL) != 0) {
        free(track->scratch);
        free(track);
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    track->owner = audio;
    track->format = config->format;
    track->volume_factor_milli = config->volume_factor_milli;
    track->scratch_values = values;
    unsigned int latency_us = 20000u;
    if (config->buffer_frames != 0u) {
        const uint64_t frames =
            (uint64_t)config->buffer_frames * config->format.frame_samples_per_channel;
        uint64_t requested = frames * UINT64_C(1000000) / config->format.sample_rate_hz;
        if (requested < 20000u) requested = 20000u;
        if (requested > 500000u) requested = 500000u;
        latency_us = (unsigned int)requested;
    }
    if (s_symbols.pcm_open(
            &track->pcm,
            device,
            H2_SND_PCM_STREAM_PLAYBACK,
            H2_SND_PCM_NONBLOCK) < 0 ||
        s_symbols.pcm_set_params(
            track->pcm,
            H2_SND_PCM_FORMAT_S16_LE,
            H2_SND_PCM_ACCESS_RW_INTERLEAVED,
            config->format.channels,
            config->format.sample_rate_hz,
            0,
            latency_us) < 0) {
        if (track->pcm != NULL) (void)s_symbols.pcm_close(track->pcm);
        (void)pthread_mutex_destroy(&track->mutex);
        free(track->scratch);
        free(track);
        return H2_AUDIO_ERR_IO;
    }
    track->api = (h2_pal_audio_track_t){
        .user = track,
        .audio = h2_linux_alsa_audio_api(),
        .write = track_write,
        .close = track_close,
        .get_volume_factor = track_get_volume,
        .set_volume_factor = track_set_volume,
        .drain = track_drain,
    };
    (void)pthread_mutex_lock(&audio->mutex);
    if (!audio->speaker_started || audio->track != NULL) {
        (void)pthread_mutex_unlock(&audio->mutex);
        (void)track_close(&track->api);
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    audio->track = &track->api;
    *out_track = &track->api;
    (void)pthread_mutex_unlock(&audio->mutex);
    return H2_AUDIO_OK;
}

static int audio_get_speaker_volume(void *user, uint32_t *out_percent) {
    linux_alsa_state_t *audio = user;
    (void)pthread_mutex_lock(&audio->mutex);
    *out_percent = audio->speaker_volume_percent;
    (void)pthread_mutex_unlock(&audio->mutex);
    return H2_AUDIO_OK;
}

static int audio_set_speaker_volume(void *user, uint32_t percent) {
    linux_alsa_state_t *audio = user;
    if (percent > 100u) return H2_AUDIO_ERR_INVALID_ARG;
    (void)pthread_mutex_lock(&audio->mutex);
    audio->speaker_volume_percent = percent;
    (void)pthread_mutex_unlock(&audio->mutex);
    return H2_AUDIO_OK;
}

static const h2_pal_audio_vtable_t s_audio_vtable = {
    .get_info = audio_get_info,
    .start_mic = audio_start_mic,
    .stop_mic = audio_stop_mic,
    .start_speaker = audio_start_speaker,
    .stop_speaker = audio_stop_speaker,
    .mic_read = audio_mic_read,
    .create_track = audio_create_track,
    .get_speaker_volume_percent = audio_get_speaker_volume,
    .set_speaker_volume_percent = audio_set_speaker_volume,
};
static h2_pal_audio_t s_audio_api = {
    .user = &s_audio,
    .vtable = &s_audio_vtable,
};

h2_pal_audio_t *h2_linux_alsa_audio_api(void) {
    return &s_audio_api;
}
