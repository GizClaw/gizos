#include "h2_audio_mixer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct h2_audio_mixer_track_state {
    h2_pal_audio_track_t track;
    struct h2_audio_mixer_impl *owner;
    h2_pal_queue_t *queue;
    h2_pal_queue_t *drain_queue;
    uint8_t *write_item;
    uint8_t *drain_item;
    uint64_t drain_sequence;
    uint32_t operations_in_flight;
    int operation_error;
    int active;
    int closing;
    int close_in_progress;
    int closed;
    int queue_closed;
    int drain_queue_closed;
    uint32_t volume_factor_milli;
} h2_audio_mixer_track_state_t;

enum {
    H2_AUDIO_MIXER_ITEM_PCM = 0,
    H2_AUDIO_MIXER_ITEM_DRAIN = 1,
    H2_AUDIO_MIXER_CLOSE_WAIT_POLL_MS = 100,
};

typedef struct h2_audio_mixer_impl {
    h2_audio_mixer_config_t config;
    size_t frame_samples;
    size_t frame_bytes;
    h2_audio_mixer_track_state_t *tracks;
    float *accum;
    float *ref_accum;
    int16_t *mix_scratch;
    uint64_t clip_count;
    h2_pal_mutex_t *track_mutex;
    h2_pal_cond_t *track_cond;
    int closing_all;
} h2_audio_mixer_impl_t;

static int mixer_lock(h2_audio_mixer_impl_t *impl) {
    return h2_pal_mutex_lock(impl->config.sync_api, impl->track_mutex);
}

static int mixer_unlock(h2_audio_mixer_impl_t *impl) {
    return h2_pal_mutex_unlock(impl->config.sync_api, impl->track_mutex);
}

static int mixer_track_operation_begin(h2_audio_mixer_track_state_t *track) {
    if (track == NULL || track->owner == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = track->owner;
    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (!track->active || track->closing || track->closed ||
        track->queue == NULL) {
        (void)mixer_unlock(impl);
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    ++track->operations_in_flight;
    rc = mixer_unlock(impl);
    if (rc != H2_AUDIO_OK) {
        --track->operations_in_flight;
        (void)mixer_unlock(impl);
        return rc;
    }
    return H2_AUDIO_OK;
}

static int mixer_track_operation_end(h2_audio_mixer_track_state_t *track) {
    h2_audio_mixer_impl_t *impl = track->owner;
    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (track->operations_in_flight == 0u) {
        (void)mixer_unlock(impl);
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    --track->operations_in_flight;
    if (track->closing && track->operations_in_flight == 0u) {
        rc = h2_pal_cond_broadcast(impl->config.sync_api, impl->track_cond);
        if (rc != H2_AUDIO_OK) {
            track->operation_error = rc;
        }
    }
    const int unlock_rc = mixer_unlock(impl);
    return rc != H2_AUDIO_OK ? rc : unlock_rc;
}

static size_t mixer_item_kind_offset(const h2_audio_mixer_impl_t *impl) {
    return impl->frame_bytes;
}

static size_t mixer_item_sequence_offset(const h2_audio_mixer_impl_t *impl) {
    return mixer_item_kind_offset(impl) + sizeof(uint8_t);
}

static size_t mixer_item_size(const h2_audio_mixer_impl_t *impl) {
    return mixer_item_sequence_offset(impl) + sizeof(uint64_t);
}

static void *mixer_alloc(h2_audio_mixer_impl_t *impl, size_t size) {
    if (impl->config.allocator != NULL) {
        return h2_pal_mem_alloc(impl->config.allocator, size);
    }
    return calloc(1u, size);
}

static void mixer_free(h2_audio_mixer_impl_t *impl, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (impl->config.allocator != NULL) {
        h2_pal_mem_free(impl->config.allocator, ptr);
        return;
    }
    free(ptr);
}

static int16_t clamp_i16(float value, uint64_t *clip_count) {
    if (value != value) {
        return 0;
    }
    if (value > 32767.0f) {
        if (clip_count != NULL) {
            ++*clip_count;
        }
        return INT16_MAX;
    }
    if (value < -32768.0f) {
        if (clip_count != NULL) {
            ++*clip_count;
        }
        return INT16_MIN;
    }
    return (int16_t)value;
}

static int mixer_frame_matches(const h2_audio_pcm_format_t *format, const h2_audio_frame_t *frame) {
    return frame->sample_rate_hz == format->sample_rate_hz &&
        frame->channels == format->channels &&
        frame->sample_format == format->sample_format;
}

static int mixer_queue_result_to_audio(int rc) {
    switch (rc) {
    case H2_PAL_QUEUE_OK:
        return H2_AUDIO_OK;
    case H2_PAL_QUEUE_ERR_INVALID_ARG:
        return H2_AUDIO_ERR_INVALID_ARG;
    case H2_PAL_QUEUE_ERR_NO_MEMORY:
        return H2_AUDIO_ERR_NO_MEMORY;
    case H2_PAL_QUEUE_ERR_TIMEOUT:
        return H2_AUDIO_ERR_WOULD_BLOCK;
    case H2_PAL_QUEUE_ERR_CLOSED:
        return H2_AUDIO_ERR_INVALID_STATE;
    default:
        return H2_AUDIO_ERR_IO;
    }
}

static void mixer_destroy_track_queue(h2_audio_mixer_track_state_t *track) {
    if (track->owner == NULL) {
        return;
    }
    h2_audio_mixer_impl_t *impl = track->owner;
    if (track->queue != NULL) {
        h2_pal_queue_destroy(impl->config.queue_api, track->queue);
        track->queue = NULL;
    }
    if (track->drain_queue != NULL) {
        h2_pal_queue_destroy(impl->config.queue_api, track->drain_queue);
        track->drain_queue = NULL;
    }
    mixer_free(impl, track->write_item);
    track->write_item = NULL;
    mixer_free(impl, track->drain_item);
    track->drain_item = NULL;
}

static int mixer_track_write(h2_pal_audio_track_t *track, const h2_audio_frame_t *frame, uint32_t timeout_ms) {
    h2_audio_mixer_track_state_t *state = (h2_audio_mixer_track_state_t *)track->user;
    if (state == NULL || frame == NULL || frame->data == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = state->owner;
    int rc = mixer_track_operation_begin(state);
    if (rc != H2_AUDIO_OK) return rc;
    if (!mixer_frame_matches(&impl->config.format, frame)) {
        rc = H2_AUDIO_ERR_UNSUPPORTED;
        goto done;
    }
    if (frame->bytes == 0u || impl->frame_bytes == 0u || (frame->bytes % impl->frame_bytes) != 0u) {
        rc = H2_AUDIO_ERR_INVALID_ARG;
        goto done;
    }

    const uint8_t *cursor = (const uint8_t *)frame->data;
    const size_t item_count = frame->bytes / impl->frame_bytes;
    rc = H2_AUDIO_OK;
    for (size_t i = 0u; i < item_count; ++i) {
        memcpy(state->write_item, cursor + i * impl->frame_bytes,
               impl->frame_bytes);
        state->write_item[mixer_item_kind_offset(impl)] =
            H2_AUDIO_MIXER_ITEM_PCM;
        const int queue_rc = h2_pal_queue_send(
            impl->config.queue_api,
            state->queue,
            state->write_item,
            timeout_ms);
        if (queue_rc != H2_PAL_QUEUE_OK) {
            rc = mixer_queue_result_to_audio(queue_rc);
            break;
        }
    }
done:
    {
        const int end_rc = mixer_track_operation_end(state);
        return rc != H2_AUDIO_OK ? rc : end_rc;
    }
}

static int mixer_track_drain(h2_pal_audio_track_t *track, uint32_t timeout_ms) {
    h2_audio_mixer_track_state_t *state = (h2_audio_mixer_track_state_t *)track->user;
    if (state == NULL || state->owner == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = state->owner;
    int rc = mixer_track_operation_begin(state);
    if (rc != H2_AUDIO_OK) return rc;
    if (state->drain_queue == NULL || state->drain_item == NULL) {
        rc = H2_AUDIO_ERR_INVALID_STATE;
        goto done;
    }
    const uint64_t sequence = ++state->drain_sequence;
    memset(state->drain_item, 0, mixer_item_size(impl));
    state->drain_item[mixer_item_kind_offset(impl)] =
        H2_AUDIO_MIXER_ITEM_DRAIN;
    memcpy(state->drain_item + mixer_item_sequence_offset(impl), &sequence,
           sizeof(sequence));
    rc = h2_pal_queue_send(
        impl->config.queue_api, state->queue, state->drain_item, timeout_ms);
    if (rc != H2_PAL_QUEUE_OK) {
        rc = mixer_queue_result_to_audio(rc);
        goto done;
    }
    for (;;) {
        uint64_t acknowledgement = 0u;
        rc = h2_pal_queue_recv(impl->config.queue_api, state->drain_queue,
                               &acknowledgement, timeout_ms);
        if (rc != H2_PAL_QUEUE_OK) {
            rc = mixer_queue_result_to_audio(rc);
            break;
        }
        if (acknowledgement == sequence) {
            rc = H2_AUDIO_OK;
            break;
        }
    }
done:
    {
        const int end_rc = mixer_track_operation_end(state);
        return rc != H2_AUDIO_OK ? rc : end_rc;
    }
}

static int mixer_track_close(h2_pal_audio_track_t *track) {
    h2_audio_mixer_track_state_t *state = (h2_audio_mixer_track_state_t *)track->user;
    if (state == NULL || state->owner == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = state->owner;
    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) return rc;
    for (;;) {
        if (!state->active && state->closed && !state->closing &&
            state->queue == NULL && state->drain_queue == NULL) {
            return mixer_unlock(impl);
        }
        if (!state->active && !state->closing) {
            (void)mixer_unlock(impl);
            return H2_AUDIO_ERR_INVALID_STATE;
        }
        if (!state->close_in_progress) {
            break;
        }
        rc = h2_pal_cond_wait(impl->config.sync_api, impl->track_cond,
                              impl->track_mutex,
                              H2_AUDIO_MIXER_CLOSE_WAIT_POLL_MS);
        if (rc != H2_AUDIO_OK && rc != H2_PAL_ERR_TIMEOUT) {
            (void)mixer_unlock(impl);
            return rc;
        }
    }
    state->closing = 1;
    state->close_in_progress = 1;
    int first_error = H2_AUDIO_OK;
    if (state->queue != NULL && !state->queue_closed) {
        const int queue_rc = h2_pal_queue_close(impl->config.queue_api, state->queue);
        if (queue_rc == H2_PAL_QUEUE_OK) {
            state->queue_closed = 1;
        } else {
            first_error = mixer_queue_result_to_audio(queue_rc);
        }
    }
    if (state->drain_queue != NULL && !state->drain_queue_closed) {
        const int queue_rc = h2_pal_queue_close(impl->config.queue_api, state->drain_queue);
        if (queue_rc == H2_PAL_QUEUE_OK) {
            state->drain_queue_closed = 1;
        } else if (first_error == H2_AUDIO_OK) {
            first_error = mixer_queue_result_to_audio(queue_rc);
        }
    }
    if (first_error != H2_AUDIO_OK) {
        state->close_in_progress = 0;
        (void)h2_pal_cond_broadcast(impl->config.sync_api, impl->track_cond);
        const int unlock_rc = mixer_unlock(impl);
        return unlock_rc != H2_AUDIO_OK ? unlock_rc : first_error;
    }
    while (state->operations_in_flight > 0u) {
        rc = h2_pal_cond_wait(impl->config.sync_api, impl->track_cond,
                              impl->track_mutex,
                              H2_AUDIO_MIXER_CLOSE_WAIT_POLL_MS);
        if (rc != H2_AUDIO_OK && rc != H2_PAL_ERR_TIMEOUT) {
            state->close_in_progress = 0;
            (void)h2_pal_cond_broadcast(impl->config.sync_api,
                                        impl->track_cond);
            (void)mixer_unlock(impl);
            return rc;
        }
    }
    if (state->operation_error != H2_AUDIO_OK) {
        const int operation_error = state->operation_error;
        state->operation_error = H2_AUDIO_OK;
        state->close_in_progress = 0;
        (void)h2_pal_cond_broadcast(impl->config.sync_api, impl->track_cond);
        const int unlock_rc = mixer_unlock(impl);
        return unlock_rc != H2_AUDIO_OK ? unlock_rc : operation_error;
    }
    mixer_destroy_track_queue(state);
    state->active = 0;
    state->close_in_progress = 0;
    state->closed = 1;
    state->queue_closed = 0;
    state->drain_queue_closed = 0;
    rc = h2_pal_cond_broadcast(impl->config.sync_api, impl->track_cond);
    if (rc != H2_AUDIO_OK) {
        (void)mixer_unlock(impl);
        return rc;
    }
    state->closing = 0;
    rc = mixer_unlock(impl);
    return rc;
}

static int mixer_track_get_volume_factor(h2_pal_audio_track_t *track, uint32_t *out_factor_milli) {
    h2_audio_mixer_track_state_t *state = (h2_audio_mixer_track_state_t *)track->user;
    if (state == NULL || state->owner == NULL || out_factor_milli == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = state->owner;
    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) return rc;
    if (!state->active || state->closing || state->closed) {
        (void)mixer_unlock(impl);
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    *out_factor_milli = state->volume_factor_milli;
    return mixer_unlock(impl);
}

static int mixer_track_set_volume_factor(h2_pal_audio_track_t *track, uint32_t factor_milli) {
    h2_audio_mixer_track_state_t *state = (h2_audio_mixer_track_state_t *)track->user;
    if (state == NULL || state->owner == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = state->owner;
    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) return rc;
    if (!state->active || state->closing || state->closed) {
        (void)mixer_unlock(impl);
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    state->volume_factor_milli = factor_milli;
    return mixer_unlock(impl);
}

int h2_audio_mixer_init(h2_audio_mixer_t *mixer, const h2_audio_mixer_config_t *config) {
    if (mixer == NULL || config == NULL || config->max_tracks == 0u ||
        config->track_queue_frames == 0u || config->queue_api == NULL ||
        config->sync_api == NULL ||
        config->format.sample_format != H2_AUDIO_SAMPLE_S16LE ||
        config->format.channels == 0u || config->format.sample_rate_hz == 0u ||
        config->format.frame_samples_per_channel == 0u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }

    mixer->impl = NULL;
    h2_audio_mixer_impl_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.config = *config;

    h2_audio_mixer_impl_t *impl = (h2_audio_mixer_impl_t *)mixer_alloc(&tmp, sizeof(*impl));
    if (impl == NULL) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    memset(impl, 0, sizeof(*impl));
    impl->config = *config;
    if (impl->config.master_factor_milli == 0u) {
        impl->config.master_factor_milli = 1000u;
    }
    impl->frame_samples = (size_t)config->format.frame_samples_per_channel * (size_t)config->format.channels;
    impl->frame_bytes = impl->frame_samples * sizeof(int16_t);

    const h2_pal_mutex_config_t mutex_config = {
        .name = "audio-mixer-tracks",
        .allocator = config->allocator,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    int rc = h2_pal_mutex_create(config->sync_api, &mutex_config,
                                 &impl->track_mutex);
    if (rc != H2_AUDIO_OK) {
        mixer_free(impl, impl);
        return rc;
    }
    const h2_pal_cond_config_t cond_config = {
        .name = "audio-mixer-tracks",
        .allocator = config->allocator,
    };
    rc = h2_pal_cond_create(config->sync_api, &cond_config, &impl->track_cond);
    if (rc != H2_AUDIO_OK) {
        (void)h2_pal_mutex_destroy(config->sync_api, impl->track_mutex);
        mixer_free(impl, impl);
        return rc;
    }

    impl->tracks = (h2_audio_mixer_track_state_t *)mixer_alloc(impl, sizeof(*impl->tracks) * config->max_tracks);
    impl->accum = (float *)mixer_alloc(impl, sizeof(float) * impl->frame_samples);
    impl->ref_accum = (float *)mixer_alloc(impl, sizeof(float) * impl->frame_samples);
    impl->mix_scratch = (int16_t *)mixer_alloc(impl, mixer_item_size(impl));
    if (impl->tracks == NULL || impl->accum == NULL || impl->ref_accum == NULL || impl->mix_scratch == NULL) {
        h2_audio_mixer_t cleanup = { .impl = impl };
        h2_audio_mixer_deinit(&cleanup);
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    memset(impl->tracks, 0, sizeof(*impl->tracks) * config->max_tracks);
    mixer->impl = impl;
    return H2_AUDIO_OK;
}

void h2_audio_mixer_deinit(h2_audio_mixer_t *mixer) {
    if (mixer == NULL || mixer->impl == NULL) {
        return;
    }
    h2_audio_mixer_impl_t *impl = (h2_audio_mixer_impl_t *)mixer->impl;
    if (impl->tracks != NULL) {
        for (uint8_t i = 0u; i < impl->config.max_tracks; ++i) {
            mixer_destroy_track_queue(&impl->tracks[i]);
        }
    }
    mixer_free(impl, impl->tracks);
    mixer_free(impl, impl->accum);
    mixer_free(impl, impl->ref_accum);
    mixer_free(impl, impl->mix_scratch);
    if (impl->track_cond != NULL) {
        (void)h2_pal_cond_destroy(impl->config.sync_api, impl->track_cond);
    }
    if (impl->track_mutex != NULL) {
        (void)h2_pal_mutex_destroy(impl->config.sync_api, impl->track_mutex);
    }
    mixer_free(impl, impl);
    mixer->impl = NULL;
}

int h2_audio_mixer_create_track(
    h2_audio_mixer_t *mixer,
    const h2_pal_audio_api_t *audio,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
    if (mixer == NULL || mixer->impl == NULL || config == NULL || out_track == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    *out_track = NULL;
    h2_audio_mixer_impl_t *impl = (h2_audio_mixer_impl_t *)mixer->impl;
    if (config->format.sample_rate_hz != impl->config.format.sample_rate_hz ||
        config->format.channels != impl->config.format.channels ||
        config->format.sample_format != impl->config.format.sample_format ||
        config->format.frame_samples_per_channel != impl->config.format.frame_samples_per_channel) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) return rc;
    if (impl->closing_all) {
        (void)mixer_unlock(impl);
        return H2_AUDIO_ERR_UNAVAILABLE;
    }

    h2_audio_mixer_track_state_t *slot = NULL;
    for (uint8_t i = 0u; i < impl->config.max_tracks; ++i) {
        if (!impl->tracks[i].active && !impl->tracks[i].closing) {
            slot = &impl->tracks[i];
            break;
        }
    }
    if (slot == NULL) {
        (void)mixer_unlock(impl);
        return H2_AUDIO_ERR_UNAVAILABLE;
    }

    mixer_destroy_track_queue(slot);
    memset(slot, 0, sizeof(*slot));
    slot->owner = impl;
    const size_t queue_frames = config->buffer_frames != 0u ? config->buffer_frames : impl->config.track_queue_frames;
    if (queue_frames == 0u) {
        (void)mixer_unlock(impl);
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_pal_queue_config_t queue_config = {
        .name = config->name,
        .item_size = mixer_item_size(impl),
        .item_count = queue_frames,
        .allocator = impl->config.allocator,
    };
    rc = h2_pal_queue_create(impl->config.queue_api, &queue_config, &slot->queue);
    if (rc != H2_PAL_QUEUE_OK) {
        memset(slot, 0, sizeof(*slot));
        (void)mixer_unlock(impl);
        return mixer_queue_result_to_audio(rc);
    }
    const h2_pal_queue_config_t drain_queue_config = {
        .name = "audio-track-drain",
        .item_size = sizeof(uint64_t),
        .item_count = 1u,
        .allocator = impl->config.allocator,
    };
    rc = h2_pal_queue_create(
        impl->config.queue_api, &drain_queue_config, &slot->drain_queue);
    slot->write_item = (uint8_t *)mixer_alloc(impl, mixer_item_size(impl));
    slot->drain_item = (uint8_t *)mixer_alloc(impl, mixer_item_size(impl));
    if (rc != H2_PAL_QUEUE_OK || slot->write_item == NULL ||
        slot->drain_item == NULL) {
        const int result = rc != H2_PAL_QUEUE_OK
            ? mixer_queue_result_to_audio(rc)
            : H2_AUDIO_ERR_NO_MEMORY;
        mixer_destroy_track_queue(slot);
        memset(slot, 0, sizeof(*slot));
        (void)mixer_unlock(impl);
        return result;
    }

    slot->active = 1;
    slot->volume_factor_milli = config->volume_factor_milli;
    slot->track.user = slot;
    slot->track.audio = audio;
    slot->track.write = mixer_track_write;
    slot->track.close = mixer_track_close;
    slot->track.get_volume_factor = mixer_track_get_volume_factor;
    slot->track.set_volume_factor = mixer_track_set_volume_factor;
    slot->track.drain = mixer_track_drain;
    *out_track = &slot->track;
    return mixer_unlock(impl);
}

static int mix_track_frame(
    h2_audio_mixer_track_state_t *track,
    float *accum,
    float *ref_accum,
    int16_t *scratch,
    size_t sample_count) {
    h2_audio_mixer_impl_t *impl = track->owner;
    if (track->queue == NULL) {
        track->active = 0;
        return H2_AUDIO_OK;
    }

    const int rc = h2_pal_queue_recv(
        impl->config.queue_api,
        track->queue,
        scratch,
        H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_QUEUE_ERR_TIMEOUT) {
        return H2_AUDIO_OK;
    }
    if (rc == H2_PAL_QUEUE_ERR_CLOSED) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    if (rc != H2_PAL_QUEUE_OK) {
        return mixer_queue_result_to_audio(rc);
    }
    if (((uint8_t *)scratch)[mixer_item_kind_offset(impl)] ==
        H2_AUDIO_MIXER_ITEM_DRAIN) {
        uint64_t acknowledgement = 0u;
        memcpy(&acknowledgement,
               (uint8_t *)scratch + mixer_item_sequence_offset(impl),
               sizeof(acknowledgement));
        const int ack_rc = h2_pal_queue_send_latest(
            impl->config.queue_api, track->drain_queue, &acknowledgement);
        return mixer_queue_result_to_audio(ack_rc);
    }

    const float factor = (float)track->volume_factor_milli / 1000.0f;
    for (size_t i = 0u; i < sample_count; ++i) {
        const float sample = (float)scratch[i] * factor;
        accum[i] += sample;
        if (ref_accum != NULL) {
            ref_accum[i] += sample;
        }
    }
    return H2_AUDIO_OK;
}

int h2_audio_mixer_read_with_reference(
    h2_audio_mixer_t *mixer,
    h2_audio_frame_t *out_frame,
    h2_audio_frame_t *ref_frame) {
    if (mixer == NULL || mixer->impl == NULL || out_frame == NULL || out_frame->data == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = (h2_audio_mixer_impl_t *)mixer->impl;
    if (!mixer_frame_matches(&impl->config.format, out_frame)) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    if (ref_frame != NULL && (ref_frame->data == NULL || !mixer_frame_matches(&impl->config.format, ref_frame))) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }

    const size_t frame_bytes = h2_audio_frame_frame_bytes(out_frame);
    if (frame_bytes == 0u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    size_t max_frames = out_frame->samples_per_channel;
    const size_t cap_frames = out_frame->capacity / frame_bytes;
    if (max_frames > cap_frames) {
        max_frames = cap_frames;
    }
    if (max_frames > impl->config.format.frame_samples_per_channel) {
        max_frames = impl->config.format.frame_samples_per_channel;
    }
    const size_t samples_to_read = max_frames * (size_t)out_frame->channels;
    if (samples_to_read == 0u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    if (ref_frame != NULL && ref_frame->capacity < samples_to_read * sizeof(int16_t)) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }

    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) return rc;
    memset(impl->accum, 0, sizeof(float) * samples_to_read);
    if (ref_frame != NULL) {
        memset(impl->ref_accum, 0, sizeof(float) * samples_to_read);
    }
    for (uint8_t i = 0u; i < impl->config.max_tracks; ++i) {
        if (!impl->tracks[i].active || impl->tracks[i].closing) {
            continue;
        }
        rc = mix_track_frame(
            &impl->tracks[i],
            impl->accum,
            ref_frame != NULL ? impl->ref_accum : NULL,
            impl->mix_scratch,
            samples_to_read);
        if (rc != H2_AUDIO_OK) {
            (void)mixer_unlock(impl);
            return rc;
        }
    }

    int16_t *out = (int16_t *)out_frame->data;
    int16_t *ref = ref_frame != NULL ? (int16_t *)ref_frame->data : NULL;
    const float master_factor = (float)impl->config.master_factor_milli / 1000.0f;
    for (size_t i = 0u; i < samples_to_read; ++i) {
        out[i] = clamp_i16(impl->accum[i] * master_factor, &impl->clip_count);
        if (ref != NULL) {
            ref[i] = clamp_i16(impl->ref_accum[i] * master_factor, NULL);
        }
    }
    out_frame->bytes = samples_to_read * sizeof(int16_t);
    if (ref_frame != NULL) {
        ref_frame->bytes = out_frame->bytes;
    }
    return mixer_unlock(impl);
}

int h2_audio_mixer_read(h2_audio_mixer_t *mixer, h2_audio_frame_t *out_frame) {
    return h2_audio_mixer_read_with_reference(mixer, out_frame, NULL);
}

int h2_audio_mixer_close_all(h2_audio_mixer_t *mixer) {
    if (mixer == NULL || mixer->impl == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = (h2_audio_mixer_impl_t *)mixer->impl;
    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) return rc;
    if (impl->closing_all) {
        (void)mixer_unlock(impl);
        return H2_AUDIO_ERR_UNAVAILABLE;
    }
    impl->closing_all = 1;
    rc = mixer_unlock(impl);
    if (rc != H2_AUDIO_OK) return rc;
    int first_error = H2_AUDIO_OK;
    for (uint8_t i = 0u; i < impl->config.max_tracks; ++i) {
        rc = mixer_lock(impl);
        if (rc != H2_AUDIO_OK) {
            if (first_error == H2_AUDIO_OK) first_error = rc;
            break;
        }
        const int needs_close =
            impl->tracks[i].active || impl->tracks[i].closing;
        rc = mixer_unlock(impl);
        if (rc != H2_AUDIO_OK) {
            if (first_error == H2_AUDIO_OK) first_error = rc;
            break;
        }
        if (needs_close) {
            const int close_rc = mixer_track_close(&impl->tracks[i].track);
            if (close_rc != H2_AUDIO_OK && first_error == H2_AUDIO_OK)
                first_error = close_rc;
        }
    }
    rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) return first_error != H2_AUDIO_OK ? first_error : rc;
    impl->closing_all = 0;
    rc = mixer_unlock(impl);
    if (rc != H2_AUDIO_OK && first_error == H2_AUDIO_OK) first_error = rc;
    return first_error;
}

int h2_audio_mixer_get_stats(h2_audio_mixer_t *mixer, h2_audio_mixer_stats_t *out_stats) {
    if (mixer == NULL || mixer->impl == NULL || out_stats == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    h2_audio_mixer_impl_t *impl = (h2_audio_mixer_impl_t *)mixer->impl;
    int rc = mixer_lock(impl);
    if (rc != H2_AUDIO_OK) return rc;
    out_stats->master_factor_milli = impl->config.master_factor_milli;
    out_stats->clip_count = impl->clip_count;
    return mixer_unlock(impl);
}
