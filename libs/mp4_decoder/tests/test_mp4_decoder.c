#include "h2_mp4_decoder.h"
#include "h2_mp4_decoder_sizing.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_state test_state_t;

typedef struct memory_source {
    const uint8_t *data;
    size_t size;
} memory_source_t;

struct h2_pal_video_decoder_frame {
    struct h2_pal_video_decoder_session *owner;
};

struct h2_pal_video_decoder_session {
    h2_pal_mem_api_t allocator;
    uint16_t *pixels;
    size_t pixel_bytes;
    size_t stride_bytes;
    uint32_t width;
    uint32_t height;
    int64_t pts_us;
    int64_t duration_us;
    int configured;
    int ready;
    int acquired;
    int eos;
    h2_pal_video_decoder_frame_t frame;
};

struct h2_pal_audio_decoder_frame {
    struct h2_pal_audio_decoder_session *owner;
};

struct h2_pal_audio_decoder_session {
    h2_pal_mem_api_t allocator;
    int16_t *samples;
    size_t sample_bytes;
    uint32_t sample_rate_hz;
    uint8_t channels;
    int64_t pts_us;
    int64_t duration_us;
    int configured;
    int ready;
    int acquired;
    int eos;
    h2_pal_audio_decoder_frame_t frame;
};

struct test_state {
    size_t allocations;
    size_t video_packets;
    size_t video_submit_attempts;
    size_t video_acquires;
    size_t timed_video_acquires;
    size_t audio_packets;
    size_t video_eos;
    size_t audio_eos;
    size_t video_resets;
    size_t video_closes;
    size_t audio_closes;
    int64_t audio_pts_offset_us;
    int fail_video_configure;
    int video_backpressure;
};

static void *tracked_alloc(void *user, size_t size) {
    test_state_t *state = user;
    void *pointer = malloc(size);
    if (pointer != NULL) {
        ++state->allocations;
    }
    return pointer;
}

static void tracked_free(void *user, void *pointer) {
    test_state_t *state = user;
    if (pointer != NULL) {
        assert(state->allocations != 0u);
        --state->allocations;
    }
    free(pointer);
}

static h2_pal_result_t source_read_at(
    void *user,
    uint64_t offset,
    void *buffer,
    size_t capacity,
    size_t *out_read) {
    FILE *file = user;
    if (offset > (uint64_t)LONG_MAX ||
        fseek(file, (long)offset, SEEK_SET) != 0) {
        return H2_PAL_ERR_IO;
    }
    *out_read = fread(buffer, 1u, capacity, file);
    return *out_read == capacity || feof(file) ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t memory_read_at(
    void *user,
    uint64_t offset,
    void *buffer,
    size_t capacity,
    size_t *out_read) {
    const memory_source_t *source = user;
    if (offset > source->size) {
        *out_read = 0u;
        return H2_PAL_OK;
    }
    size_t available = source->size - (size_t)offset;
    if (capacity > available) {
        capacity = available;
    }
    memcpy(buffer, source->data + (size_t)offset, capacity);
    *out_read = capacity;
    return H2_PAL_OK;
}

static h2_pal_result_t video_open(
    void *user,
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session) {
    (void)user;
    h2_pal_video_decoder_session_t *session =
        h2_pal_mem_alloc(config->frame_allocator, sizeof(*session));
    if (session == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(*session));
    session->allocator = *config->frame_allocator;
    session->frame.owner = session;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t video_configure(
    void *user,
    h2_pal_video_decoder_session_t *session,
    const h2_video_decoder_stream_config_t *config) {
    test_state_t *state = user;
    assert(config->codec == H2_VIDEO_CODEC_H264);
    assert(config->bitstream_format == H2_VIDEO_BITSTREAM_H264_ANNEX_B);
    assert(config->codec_config != NULL && config->codec_config_size != 0u);
    if (state->fail_video_configure) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    const size_t row_bytes =
        (size_t)config->visible_width * sizeof(uint16_t);
    if (row_bytes > SIZE_MAX - 384u) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    session->stride_bytes = row_bytes + 384u;
    if (config->visible_height > SIZE_MAX / session->stride_bytes) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    session->pixel_bytes =
        session->stride_bytes * config->visible_height;
    session->pixels =
        h2_pal_mem_alloc(&session->allocator, session->pixel_bytes);
    if (session->pixels == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    for (size_t i = 0u;
         i < session->pixel_bytes / sizeof(uint16_t);
         ++i) {
        session->pixels[i] = (uint16_t)(i + 1u);
    }
    session->width = config->visible_width;
    session->height = config->visible_height;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t video_submit(
    void *user,
    h2_pal_video_decoder_session_t *session,
    const h2_video_decoder_packet_t *packet) {
    test_state_t *state = user;
    if (!session->configured || session->eos) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if ((packet->flags & H2_VIDEO_DECODER_PACKET_END_OF_STREAM) != 0u) {
        session->eos = 1;
        ++state->video_eos;
        return H2_PAL_OK;
    }
    ++state->video_submit_attempts;
    if (state->video_backpressure) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (session->ready || session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    assert(packet->data != NULL && packet->size > 4u);
    const uint8_t *data = packet->data;
    assert(data[0] == 0u && data[1] == 0u);
    assert(data[2] == 1u || (data[2] == 0u && data[3] == 1u));
    session->pts_us = packet->pts_us;
    session->duration_us = packet->duration_us;
    session->ready = 1;
    ++state->video_packets;
    return H2_PAL_OK;
}

static h2_pal_result_t video_acquire(
    void *user,
    h2_pal_video_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_video_decoder_frame_t **out_frame) {
    test_state_t *state = user;
    ++state->video_acquires;
    if (state->video_backpressure && timeout_ms != 0u) {
        ++state->timed_video_acquires;
        return state->timed_video_acquires == 1u
            ? H2_PAL_ERR_TIMEOUT
            : H2_PAL_ERR_IO;
    }
    if (session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (!session->ready) {
        return session->eos ? H2_PAL_EXIT : H2_PAL_ERR_WOULD_BLOCK;
    }
    session->acquired = 1;
    *out_frame = &session->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t video_info(
    void *user,
    h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame,
    h2_video_frame_info_t *out_info) {
    (void)user;
    if (!session->acquired || frame != &session->frame) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = (h2_video_frame_info_t){
        .format = H2_VIDEO_PIXEL_FORMAT_RGB565,
        .width = session->width,
        .height = session->height,
        .planes = {{
            .data = session->pixels,
            .bytes = session->pixel_bytes,
            .stride_bytes = session->stride_bytes,
        }},
        .plane_count = 1u,
        .pts_us = session->pts_us,
        .duration_us = session->duration_us,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t video_release(
    void *user,
    h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame) {
    (void)user;
    if (!session->acquired || frame != &session->frame) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    session->acquired = 0;
    session->ready = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t video_reset(
    void *user,
    h2_pal_video_decoder_session_t *session) {
    test_state_t *state = user;
    if (session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_mem_free(&session->allocator, session->pixels);
    session->pixels = NULL;
    session->configured = 0;
    session->ready = 0;
    session->eos = 0;
    ++state->video_resets;
    return H2_PAL_OK;
}

static h2_pal_result_t video_close(
    void *user,
    h2_pal_video_decoder_session_t *session) {
    test_state_t *state = user;
    if (session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const h2_pal_mem_api_t allocator = session->allocator;
    h2_pal_mem_free(&allocator, session->pixels);
    h2_pal_mem_free(&allocator, session);
    ++state->video_closes;
    return H2_PAL_OK;
}

static const h2_pal_video_decoder_vtable_t s_video_vtable = {
    .open = video_open,
    .configure = video_configure,
    .submit_packet = video_submit,
    .acquire_frame = video_acquire,
    .frame_get_info = video_info,
    .release_frame = video_release,
    .reset = video_reset,
    .close = video_close,
};

static h2_pal_result_t audio_open(
    void *user,
    const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
    (void)user;
    h2_pal_audio_decoder_session_t *session =
        h2_pal_mem_alloc(config->pcm_allocator, sizeof(*session));
    if (session == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(*session));
    session->allocator = *config->pcm_allocator;
    session->frame.owner = session;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_configure(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_stream_config_t *config) {
    (void)user;
    assert(config->codec == H2_AUDIO_CODEC_AAC_LC);
    assert(config->bitstream_format == H2_AUDIO_BITSTREAM_AAC_RAW);
    assert(config->codec_config != NULL && config->codec_config_size != 0u);
    session->sample_bytes =
        1024u * config->channels * sizeof(int16_t);
    session->samples =
        h2_pal_mem_alloc(&session->allocator, session->sample_bytes);
    if (session->samples == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    for (size_t i = 0u;
         i < session->sample_bytes / sizeof(int16_t);
         ++i) {
        session->samples[i] = (int16_t)(i + 1u);
    }
    session->sample_rate_hz = config->sample_rate_hz;
    session->channels = config->channels;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_submit(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_packet_t *packet) {
    test_state_t *state = user;
    if (!session->configured || session->eos) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if ((packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u) {
        session->eos = 1;
        ++state->audio_eos;
        return H2_PAL_OK;
    }
    if (session->ready || session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    assert(packet->data != NULL && packet->size != 0u);
    assert(packet->pts_us <= INT64_MAX - state->audio_pts_offset_us);
    session->pts_us = packet->pts_us + state->audio_pts_offset_us;
    session->duration_us = packet->duration_us;
    session->ready = 1;
    ++state->audio_packets;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_acquire(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    (void)user;
    (void)timeout_ms;
    if (session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (!session->ready) {
        return session->eos ? H2_PAL_EXIT : H2_PAL_ERR_WOULD_BLOCK;
    }
    session->acquired = 1;
    *out_frame = &session->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_info(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame,
    h2_audio_decoder_frame_info_t *out_info) {
    (void)user;
    if (!session->acquired || frame != &session->frame) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = (h2_audio_decoder_frame_info_t){
        .data = session->samples,
        .bytes = session->sample_bytes,
        .sample_rate_hz = session->sample_rate_hz,
        .samples_per_channel = 1024u,
        .channels = session->channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .pts_us = session->pts_us,
        .duration_us = session->duration_us,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t audio_release(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame) {
    (void)user;
    if (!session->acquired || frame != &session->frame) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    session->acquired = 0;
    session->ready = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_reset(
    void *user,
    h2_pal_audio_decoder_session_t *session) {
    (void)user;
    if (session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_mem_free(&session->allocator, session->samples);
    session->samples = NULL;
    session->configured = 0;
    session->ready = 0;
    session->eos = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_close(
    void *user,
    h2_pal_audio_decoder_session_t *session) {
    test_state_t *state = user;
    if (session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const h2_pal_mem_api_t allocator = session->allocator;
    h2_pal_mem_free(&allocator, session->samples);
    h2_pal_mem_free(&allocator, session);
    ++state->audio_closes;
    return H2_PAL_OK;
}

static const h2_pal_audio_decoder_vtable_t s_audio_vtable = {
    .open = audio_open,
    .configure = audio_configure,
    .submit_packet = audio_submit,
    .acquire_frame = audio_acquire,
    .frame_get_info = audio_info,
    .release_frame = audio_release,
    .reset = audio_reset,
    .close = audio_close,
};

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(h2_mp4_decoder_open(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);

    size_t capacity = 0u;
    assert(h2_mp4_decoder_annex_b_capacity(1u, 2639u, &capacity));
    assert(capacity == 6596u);
    assert(h2_mp4_decoder_annex_b_capacity(2u, 3u, &capacity));
    assert(capacity == 5u);
    assert(h2_mp4_decoder_annex_b_capacity(3u, 4u, &capacity));
    assert(capacity == 5u);
    assert(h2_mp4_decoder_annex_b_capacity(4u, 5u, &capacity));
    assert(capacity == 5u);
    assert(!h2_mp4_decoder_annex_b_capacity(0u, 1u, &capacity));
    assert(!h2_mp4_decoder_annex_b_capacity(1u, SIZE_MAX, &capacity));

    assert(h2_mp4_decoder_video_frame_capacity(
        H2_VIDEO_PIXEL_FORMAT_NV12, 5u, 3u, &capacity));
    assert(capacity == 27u);
    assert(h2_mp4_decoder_video_frame_capacity(
        H2_VIDEO_PIXEL_FORMAT_YUV420P, 4u, 3u, &capacity));
    assert(capacity == 20u);
    assert(h2_mp4_decoder_video_frame_capacity(
        H2_VIDEO_PIXEL_FORMAT_YUV420P, 5u, 4u, &capacity));
    assert(capacity == 32u);
    assert(h2_mp4_decoder_video_frame_capacity(
        H2_VIDEO_PIXEL_FORMAT_RGB565, 5u, 3u, &capacity));
    assert(capacity == 30u);
    assert(!h2_mp4_decoder_video_frame_capacity(
        H2_VIDEO_PIXEL_FORMAT_NV12, SIZE_MAX, 2u, &capacity));

    FILE *file = fopen(argv[1], "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    const long file_size = ftell(file);
    assert(file_size > 16);

    test_state_t state = {
        .audio_pts_offset_us = 500000,
    };
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = tracked_alloc,
        .free = tracked_free,
    };
    const h2_pal_mem_api_t allocator = {
        .user = &state,
        .vtable = &mem_vtable,
    };
    const h2_pal_video_decoder_api_t video_decoder = {
        .user = &state,
        .vtable = &s_video_vtable,
    };
    const h2_pal_audio_decoder_api_t audio_decoder = {
        .user = &state,
        .vtable = &s_audio_vtable,
    };
    const h2_mp4_decoder_config_t config = {
        .allocator = &allocator,
        .source = {
            .user = file,
            .size = (uint64_t)file_size,
            .read_at = source_read_at,
        },
        .video_decoder = video_decoder,
        .audio_decoder = audio_decoder,
        .video_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
        .require_video = 1,
        .require_audio = 1,
        .max_pcm_bytes = 96u * 1024u,
    };

    h2_mp4_decoder_t *decoder = NULL;
    h2_mp4_decoder_config_t invalid = config;
    invalid.source.size = 16u;
    assert(h2_mp4_decoder_open(&invalid, &decoder) == H2_PAL_ERR_FORMAT);
    assert(decoder == NULL && state.allocations == 0u);
    invalid = config;
    invalid.max_file_bytes = (size_t)file_size - 1u;
    assert(h2_mp4_decoder_open(&invalid, &decoder) == H2_PAL_ERR_NO_MEMORY);
    assert(decoder == NULL && state.allocations == 0u);

    state.fail_video_configure = 1;
    assert(h2_mp4_decoder_open(&config, &decoder) == H2_PAL_ERR_UNSUPPORTED);
    assert(decoder == NULL && state.allocations == 0u);
    assert(state.video_closes == 1u && state.audio_closes == 0u);
    state.fail_video_configure = 0;

    assert(h2_mp4_decoder_open(&config, &decoder) == H2_PAL_OK);
    h2_mp4_decoder_info_t media = {0};
    assert(h2_mp4_decoder_get_info(decoder, &media) == H2_PAL_OK);
    assert(media.has_video && media.has_audio);
    assert(media.width == 160u && media.height == 96u);
    assert(media.audio_sample_rate_hz == 16000u);
    assert(media.audio_channels == 1u);
    assert(state.audio_packets == 33u);
    assert(state.audio_packets * 2048u * sizeof(int16_t) >
        config.max_pcm_bytes);

    size_t frames = 0u;
    size_t silent_frames = 0u;
    size_t nonzero_frames = 0u;
    for (;;) {
        h2_mp4_decoder_frame_t *frame = NULL;
        const h2_pal_result_t result =
            h2_mp4_decoder_acquire_frame(decoder, 25u, &frame);
        if (result == H2_PAL_EXIT) {
            break;
        }
        assert(result == H2_PAL_OK && frame != NULL);
        h2_mp4_decoder_frame_info_t info = {0};
        assert(h2_mp4_decoder_frame_get_info(decoder, frame, &info) ==
            H2_PAL_OK);
        assert(info.video_format == H2_VIDEO_PIXEL_FORMAT_RGB565);
        assert(info.width == 160u && info.height == 96u);
        assert(info.video_plane_count == 1u);
        assert(info.video_planes[0].data != NULL);
        assert(info.video_planes[0].stride_bytes == 704u);
        assert(info.video_planes[0].bytes >
            (size_t)info.width * info.height * 4u);
        assert(info.pcm != NULL && info.pcm_samples_per_channel != 0u);
        int nonzero = 0;
        for (size_t i = 0u; i < info.pcm_samples_per_channel; ++i) {
            nonzero |= info.pcm[i] != 0;
        }
        silent_frames += !nonzero;
        nonzero_frames += nonzero;
        if (frames == 0u) {
            h2_mp4_decoder_frame_t *blocked = NULL;
            assert(h2_mp4_decoder_acquire_frame(decoder, 0u, &blocked) ==
                H2_PAL_ERR_WOULD_BLOCK);
            assert(h2_mp4_decoder_seek(decoder, 0) ==
                H2_PAL_ERR_INVALID_STATE);
        }
        assert(h2_mp4_decoder_release_frame(decoder, frame) == H2_PAL_OK);
        ++frames;
    }
    assert(frames == 48u);
    assert(silent_frames != 0u && nonzero_frames != 0u);
    assert(state.video_packets == 48u && state.video_eos == 1u);
    assert(state.audio_packets == 33u && state.audio_eos == 1u);

    assert(h2_mp4_decoder_seek(decoder, 0) == H2_PAL_OK);
    assert(state.video_resets == 1u);
    h2_mp4_decoder_frame_t *first_again = NULL;
    assert(h2_mp4_decoder_acquire_frame(decoder, 25u, &first_again) ==
        H2_PAL_OK);
    assert(h2_mp4_decoder_release_frame(decoder, first_again) == H2_PAL_OK);
    assert(h2_mp4_decoder_close(decoder) == H2_PAL_OK);
    assert(state.allocations == 0u);
    assert(state.video_closes == 2u && state.audio_closes == 1u);

    test_state_t blocked_state = {
        .audio_pts_offset_us = 500000,
        .video_backpressure = 1,
    };
    const h2_pal_mem_api_t blocked_allocator = {
        .user = &blocked_state,
        .vtable = &mem_vtable,
    };
    const h2_pal_video_decoder_api_t blocked_video_decoder = {
        .user = &blocked_state,
        .vtable = &s_video_vtable,
    };
    const h2_pal_audio_decoder_api_t blocked_audio_decoder = {
        .user = &blocked_state,
        .vtable = &s_audio_vtable,
    };
    h2_mp4_decoder_config_t blocked_config = config;
    blocked_config.allocator = &blocked_allocator;
    blocked_config.video_decoder = blocked_video_decoder;
    blocked_config.audio_decoder = blocked_audio_decoder;
    decoder = NULL;
    assert(h2_mp4_decoder_open(&blocked_config, &decoder) == H2_PAL_OK);
    h2_mp4_decoder_frame_t *blocked_frame = NULL;
    assert(h2_mp4_decoder_acquire_frame(
               decoder, 25u, &blocked_frame) == H2_PAL_ERR_TIMEOUT);
    assert(blocked_frame == NULL);
    assert(blocked_state.video_acquires == 2u);
    assert(blocked_state.timed_video_acquires == 1u);
    assert(blocked_state.video_submit_attempts == 1u);
    assert(h2_mp4_decoder_close(decoder) == H2_PAL_OK);
    assert(blocked_state.allocations == 0u);

    uint8_t *malformed = malloc((size_t)file_size);
    assert(malformed != NULL);
    assert(fseek(file, 0, SEEK_SET) == 0);
    assert(fread(malformed, 1u, (size_t)file_size, file) ==
        (size_t)file_size);
    size_t stco_type_offset = SIZE_MAX;
    for (size_t i = 0u; i + 12u <= (size_t)file_size; ++i) {
        if (memcmp(malformed + i, "stco", 4u) == 0) {
            stco_type_offset = i;
            break;
        }
    }
    assert(stco_type_offset != SIZE_MAX);
    memset(malformed + stco_type_offset + 8u, 0xff, sizeof(uint32_t));
    memory_source_t malformed_source = {
        .data = malformed,
        .size = (size_t)file_size,
    };
    h2_mp4_decoder_config_t malformed_config = config;
    malformed_config.source = (h2_mp4_decoder_source_api_t){
        .user = &malformed_source,
        .size = (uint64_t)malformed_source.size,
        .read_at = memory_read_at,
    };
    decoder = NULL;
    assert(h2_mp4_decoder_open(&malformed_config, &decoder) ==
        H2_PAL_ERR_UNSUPPORTED);
    assert(decoder == NULL && state.allocations == 0u);
    free(malformed);

    fclose(file);
    return 0;
}
