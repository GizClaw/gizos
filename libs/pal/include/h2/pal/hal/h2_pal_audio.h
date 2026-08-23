#ifndef H2_PAL_AUDIO_H
#define H2_PAL_AUDIO_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_audio_sample_format {
    H2_AUDIO_SAMPLE_S16LE = 1,
} h2_audio_sample_format_t;

typedef struct h2_audio_pcm_format {
    uint32_t sample_rate_hz;
    uint16_t frame_samples_per_channel;
    uint8_t channels;
    h2_audio_sample_format_t sample_format;
} h2_audio_pcm_format_t;

typedef struct h2_audio_frame {
    void *data;
    size_t capacity;
    size_t bytes;
    uint32_t sample_rate_hz;
    uint16_t samples_per_channel;
    uint8_t channels;
    h2_audio_sample_format_t sample_format;
} h2_audio_frame_t;

typedef struct h2_audio_info {
    int available;
    int mic_supported;
    int playback_supported;
    h2_audio_pcm_format_t mic_format;
    h2_audio_pcm_format_t playback_format;
    uint8_t mic_queue_frames;
    uint8_t track_queue_frames;
    uint8_t max_tracks;
} h2_audio_info_t;

typedef struct h2_audio_track_config {
    const char *name;
    h2_audio_pcm_format_t format;
    uint32_t volume_factor_milli;
    size_t buffer_frames;
} h2_audio_track_config_t;

typedef struct h2_pal_audio_api h2_pal_audio_api_t;
typedef h2_pal_audio_api_t h2_pal_audio_t;
typedef struct h2_pal_audio_track h2_pal_audio_track_t;

typedef struct h2_pal_audio_vtable {
    int (*get_info)(void *user, h2_audio_info_t *info);
    int (*start_mic)(void *user);
    int (*stop_mic)(void *user);
    int (*start_speaker)(void *user);
    int (*stop_speaker)(void *user);
    int (*mic_read)(void *user, h2_audio_frame_t *out_frame, uint32_t timeout_ms);
    int (*create_track)(
        void *user,
        const h2_audio_track_config_t *config,
        h2_pal_audio_track_t **out_track);
    int (*get_speaker_volume_percent)(void *user, uint32_t *out_percent);
    int (*set_speaker_volume_percent)(void *user, uint32_t percent);
} h2_pal_audio_vtable_t;

typedef int (*h2_pal_audio_track_write_fn)(
    h2_pal_audio_track_t *track,
    const h2_audio_frame_t *frame,
    uint32_t timeout_ms);
typedef int (*h2_pal_audio_track_close_fn)(h2_pal_audio_track_t *track);
typedef int (*h2_pal_audio_track_get_volume_factor_fn)(h2_pal_audio_track_t *track, uint32_t *out_factor_milli);
typedef int (*h2_pal_audio_track_set_volume_factor_fn)(h2_pal_audio_track_t *track, uint32_t factor_milli);
typedef int (*h2_pal_audio_track_drain_fn)(h2_pal_audio_track_t *track, uint32_t timeout_ms);

struct h2_pal_audio_track {
    void *user;
    const h2_pal_audio_api_t *audio;
    h2_pal_audio_track_write_fn write;
    h2_pal_audio_track_close_fn close;
    h2_pal_audio_track_get_volume_factor_fn get_volume_factor;
    h2_pal_audio_track_set_volume_factor_fn set_volume_factor;
    h2_pal_audio_track_drain_fn drain;
};

struct h2_pal_audio_api {
    void *user;
    const h2_pal_audio_vtable_t *vtable;
};

static inline size_t h2_audio_pcm_frame_bytes(const h2_audio_pcm_format_t *format) {
    if (format == NULL || format->sample_format != H2_AUDIO_SAMPLE_S16LE || format->channels == 0u) {
        return 0u;
    }
    return (size_t)format->channels * sizeof(int16_t);
}

static inline size_t h2_audio_frame_frame_bytes(const h2_audio_frame_t *frame) {
    if (frame == NULL || frame->sample_format != H2_AUDIO_SAMPLE_S16LE || frame->channels == 0u) {
        return 0u;
    }
    return (size_t)frame->channels * sizeof(int16_t);
}

static inline h2_audio_frame_t h2_audio_frame_for_buffer(
    void *data,
    size_t capacity,
    h2_audio_pcm_format_t format) {
    h2_audio_frame_t frame;
    frame.data = data;
    frame.capacity = capacity;
    frame.bytes = 0u;
    frame.sample_rate_hz = format.sample_rate_hz;
    frame.samples_per_channel = format.frame_samples_per_channel;
    frame.channels = format.channels;
    frame.sample_format = format.sample_format;
    return frame;
}

static inline int h2_pal_audio_get_info(const h2_pal_audio_api_t *audio, h2_audio_info_t *info) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->get_info == NULL || info == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return audio->vtable->get_info(audio->user, info);
}

static inline int h2_pal_audio_start_mic(const h2_pal_audio_api_t *audio) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->start_mic == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return audio->vtable->start_mic(audio->user);
}

static inline int h2_pal_audio_stop_mic(const h2_pal_audio_api_t *audio) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->stop_mic == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return audio->vtable->stop_mic(audio->user);
}

static inline int h2_pal_audio_start_speaker(const h2_pal_audio_api_t *audio) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->start_speaker == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return audio->vtable->start_speaker(audio->user);
}

static inline int h2_pal_audio_stop_speaker(const h2_pal_audio_api_t *audio) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->stop_speaker == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return audio->vtable->stop_speaker(audio->user);
}

static inline int h2_pal_audio_mic_read(
    const h2_pal_audio_api_t *audio,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->mic_read == NULL || out_frame == NULL ||
        out_frame->data == NULL || out_frame->capacity == 0u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    out_frame->bytes = 0u;
    return audio->vtable->mic_read(audio->user, out_frame, timeout_ms);
}

static inline int h2_pal_audio_create_track(
    const h2_pal_audio_api_t *audio,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->create_track == NULL ||
        config == NULL || out_track == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    *out_track = NULL;
    return audio->vtable->create_track(audio->user, config, out_track);
}

static inline int h2_pal_audio_get_speaker_volume_percent(const h2_pal_audio_api_t *audio, uint32_t *out_percent) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->get_speaker_volume_percent == NULL ||
        out_percent == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return audio->vtable->get_speaker_volume_percent(audio->user, out_percent);
}

static inline int h2_pal_audio_set_speaker_volume_percent(const h2_pal_audio_api_t *audio, uint32_t percent) {
    if (audio == NULL || audio->vtable == NULL || audio->vtable->set_speaker_volume_percent == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return audio->vtable->set_speaker_volume_percent(audio->user, percent);
}

static inline int h2_pal_audio_track_write(
    h2_pal_audio_track_t *track,
    const h2_audio_frame_t *frame,
    uint32_t timeout_ms) {
    if (track == NULL || track->write == NULL || frame == NULL ||
        frame->data == NULL || frame->bytes == 0u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return track->write(track, frame, timeout_ms);
}

static inline int h2_pal_audio_track_close(h2_pal_audio_track_t *track) {
    if (track == NULL || track->close == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return track->close(track);
}

static inline int h2_pal_audio_track_get_volume_factor(h2_pal_audio_track_t *track, uint32_t *out_factor_milli) {
    if (track == NULL || track->get_volume_factor == NULL || out_factor_milli == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return track->get_volume_factor(track, out_factor_milli);
}

static inline int h2_pal_audio_track_set_volume_factor(h2_pal_audio_track_t *track, uint32_t factor_milli) {
    if (track == NULL || track->set_volume_factor == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return track->set_volume_factor(track, factor_milli);
}

static inline int h2_pal_audio_track_drain(h2_pal_audio_track_t *track, uint32_t timeout_ms) {
    if (track == NULL || track->drain == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return track->drain(track, timeout_ms);
}

#ifdef __cplusplus
}
#endif

#endif
