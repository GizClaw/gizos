#ifndef H2_PAL_AUDIO_H
#define H2_PAL_AUDIO_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

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
    /* Optional per-track storage; NULL uses the audio provider default.
     * Must remain valid until track close completes. */
    const h2_pal_mem_api_t *allocator;
} h2_audio_track_config_t;

typedef struct h2_pal_audio_api h2_pal_audio_api_t;
typedef h2_pal_audio_api_t h2_pal_audio_t;
typedef struct h2_pal_audio_track h2_pal_audio_track_t;

/** One complete, time-aligned AEC processing frame. PCM is borrowed only for
 * on_capture; do not retain it. raw contains interleaved ADC samples before
 * AEC; microphone_lane and reference_lane identify actual packed lanes, not
 * codec input numbers. reference_input is the mono reference after provider
 * scaling, exactly as passed to AEC; processed is the resulting mono output.
 * All formats have equal sample rates and sample counts. sequence increases
 * for every processed frame within a microphone session. */
typedef struct h2_audio_aec_frame {
    uint64_t sequence;
    const h2_audio_frame_t *raw;
    const h2_audio_frame_t *reference_input;
    const h2_audio_frame_t *processed;
    uint8_t microphone_lane;
    uint8_t reference_lane;
    uint8_t microphone_mask; /**< All raw ADC lanes used as AEC microphones. */
} h2_audio_aec_frame_t;

/** Optional diagnostics for a real AEC path. The descriptor is copied during
 * registration, but user is borrowed until successful unregister. Callbacks
 * run synchronously on provider workers, cannot block, allocate or reenter
 * Audio, and cannot change PCM. on_capture and on_playback can run concurrently
 * with each other; each callback is serialized with itself. on_playback sees
 * only complete PCM actually accepted by the DAC transport, after mixing and
 * digital gain. Capture sequence and playback timing are independent; the
 * hardware reference in on_capture supplies AEC alignment. No callback is
 * delivered after the corresponding successful stop operation has joined. */
typedef struct h2_audio_aec_observer {
    void *user;
    void (*on_capture)(void *user, const h2_audio_aec_frame_t *frame);
    void (*on_playback)(void *user, const h2_audio_frame_t *frame);
    /** Nonzero capture selects the mic worker; zero selects playback. Reports
     * actual processing/transport errors, including incomplete DAC writes.
     * Ordinary empty capture queues are not errors. Same borrowing and
     * per-worker serialization as the corresponding PCM callback. */
    void (*on_error)(void *user, int capture, int result);
} h2_audio_aec_observer_t;

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
    int (*get_mic_gain_percent)(void *user, uint32_t *out_percent);
    int (*set_mic_gain_percent)(void *user, uint32_t percent);
    int (*set_aec_observer)(void *user, const h2_audio_aec_observer_t *observer);
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

/**
 * @brief Read the current microphone gain on the provider's 0..100 scale.
 *
 * The board or provider owns the mapping to codec gain or PCM scaling. The
 * result is zeroed on failure. An audio device without adjustable microphone
 * gain returns H2_AUDIO_ERR_UNSUPPORTED. Hardware may quantize the setting,
 * so the returned percentage can differ from the last request.
 */
static inline int h2_pal_audio_get_mic_gain_percent(
    const h2_pal_audio_api_t *audio, uint32_t *out_percent) {
    if (out_percent == NULL) return H2_AUDIO_ERR_INVALID_ARG;
    *out_percent = 0u;
    if (audio == NULL || audio->vtable == NULL) return H2_AUDIO_ERR_INVALID_ARG;
    if (audio->vtable->get_mic_gain_percent == NULL) return H2_AUDIO_ERR_UNSUPPORTED;
    uint32_t current = 0u;
    const int rc = audio->vtable->get_mic_gain_percent(audio->user, &current);
    if (rc != H2_AUDIO_OK) return rc;
    if (current > 100u) return H2_AUDIO_ERR_IO;
    *out_percent = current;
    return H2_AUDIO_OK;
}

/**
 * @brief Set microphone gain on the provider's 0..100 scale.
 *
 * The mapping and default percentage are board or provider owned. This call
 * may block for device control I/O; call it from task context and serialize
 * gain control with other Audio PAL control and lifecycle calls. Success
 * applies to subsequent captured frames without
 * restarting the microphone. Unsupported devices return UNSUPPORTED. A device
 * I/O failure keeps the last reported value; a multi-input codec may require
 * recovery if a rollback write also fails.
 */
static inline int h2_pal_audio_set_mic_gain_percent(
    const h2_pal_audio_api_t *audio, uint32_t percent) {
    if (audio == NULL || audio->vtable == NULL || percent > 100u)
        return H2_AUDIO_ERR_INVALID_ARG;
    if (audio->vtable->set_mic_gain_percent == NULL) return H2_AUDIO_ERR_UNSUPPORTED;
    return audio->vtable->set_mic_gain_percent(audio->user, percent);
}

/** Register/replace diagnostics, or unregister with NULL. Serialized task
 * context only, with microphone and speaker stopped and their workers joined.
 * A non-NULL descriptor requires all three callbacks. A provider with no actual AEC
 * or no pre/post-AEC observation returns UNSUPPORTED, including unregister.
 * Registration while running returns INVALID_STATE. Failure preserves the old
 * registration and its borrowed user; retain it and retry cleanup. An Audio
 * decorator which replaces capture cannot claim real acoustic diagnostics. */
static inline int h2_pal_audio_set_aec_observer(
    const h2_pal_audio_api_t *audio, const h2_audio_aec_observer_t *observer) {
    if (audio == NULL || audio->vtable == NULL ||
        (observer != NULL && (observer->on_capture == NULL ||
                              observer->on_playback == NULL || observer->on_error == NULL)))
        return H2_AUDIO_ERR_INVALID_ARG;
    if (audio->vtable->set_aec_observer == NULL)
        return H2_AUDIO_ERR_UNSUPPORTED;
    return audio->vtable->set_aec_observer(audio->user, observer);
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
