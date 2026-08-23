#ifndef H2_PAL_AUDIO_DECODER_H
#define H2_PAL_AUDIO_DECODER_H

#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_audio_codec {
    H2_AUDIO_CODEC_AAC_LC = 1,
} h2_audio_codec_t;

typedef enum h2_audio_bitstream_format {
    H2_AUDIO_BITSTREAM_AAC_RAW = 1,
} h2_audio_bitstream_format_t;

typedef struct h2_audio_decoder_config {
    const h2_pal_mem_api_t *pcm_allocator;
    h2_audio_sample_format_t preferred_format;
} h2_audio_decoder_config_t;

typedef struct h2_audio_decoder_stream_config {
    h2_audio_codec_t codec;
    h2_audio_bitstream_format_t bitstream_format;
    uint32_t sample_rate_hz;
    uint8_t channels;
    /** Borrowed MPEG-4 AudioSpecificConfig consumed during configure. */
    const void *codec_config;
    size_t codec_config_size;
} h2_audio_decoder_stream_config_t;

enum {
    H2_AUDIO_DECODER_PACKET_END_OF_STREAM = 1u << 0,
};

typedef struct h2_audio_decoder_packet {
    const void *data;
    size_t size;
    int64_t pts_us;
    int64_t dts_us;
    int64_t duration_us;
    uint32_t flags;
} h2_audio_decoder_packet_t;

typedef struct h2_audio_decoder_frame_info {
    const void *data;
    size_t bytes;
    uint32_t sample_rate_hz;
    uint32_t samples_per_channel;
    uint8_t channels;
    h2_audio_sample_format_t sample_format;
    int64_t pts_us;
    int64_t duration_us;
} h2_audio_decoder_frame_info_t;

typedef struct h2_pal_audio_decoder_session h2_pal_audio_decoder_session_t;
typedef struct h2_pal_audio_decoder_frame h2_pal_audio_decoder_frame_t;
typedef struct h2_pal_audio_decoder_api h2_pal_audio_decoder_api_t;

typedef struct h2_pal_audio_decoder_vtable {
    h2_pal_result_t (*open)(
        void *user,
        const h2_audio_decoder_config_t *config,
        h2_pal_audio_decoder_session_t **out_session);
    h2_pal_result_t (*configure)(
        void *user,
        h2_pal_audio_decoder_session_t *session,
        const h2_audio_decoder_stream_config_t *config);
    h2_pal_result_t (*submit_packet)(
        void *user,
        h2_pal_audio_decoder_session_t *session,
        const h2_audio_decoder_packet_t *packet);
    h2_pal_result_t (*acquire_frame)(
        void *user,
        h2_pal_audio_decoder_session_t *session,
        uint32_t timeout_ms,
        h2_pal_audio_decoder_frame_t **out_frame);
    h2_pal_result_t (*frame_get_info)(
        void *user,
        h2_pal_audio_decoder_session_t *session,
        h2_pal_audio_decoder_frame_t *frame,
        h2_audio_decoder_frame_info_t *out_info);
    h2_pal_result_t (*release_frame)(
        void *user,
        h2_pal_audio_decoder_session_t *session,
        h2_pal_audio_decoder_frame_t *frame);
    h2_pal_result_t (*reset)(void *user, h2_pal_audio_decoder_session_t *session);
    h2_pal_result_t (*close)(void *user, h2_pal_audio_decoder_session_t *session);
} h2_pal_audio_decoder_vtable_t;

struct h2_pal_audio_decoder_api {
    void *user;
    const h2_pal_audio_decoder_vtable_t *vtable;
};

static inline int h2_audio_decoder_allocator_is_valid(
    const h2_pal_mem_api_t *allocator) {
    return allocator != NULL && allocator->vtable != NULL &&
        allocator->vtable->alloc != NULL && allocator->vtable->free != NULL;
}

static inline h2_pal_result_t h2_pal_audio_decoder_open(
    const h2_pal_audio_decoder_api_t *decoder,
    const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
    if (out_session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_session = NULL;
    if (decoder == NULL || decoder->vtable == NULL || decoder->vtable->open == NULL ||
        config == NULL || !h2_audio_decoder_allocator_is_valid(config->pcm_allocator)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->open(decoder->user, config, out_session);
}

static inline h2_pal_result_t h2_pal_audio_decoder_configure(
    const h2_pal_audio_decoder_api_t *decoder,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_stream_config_t *config) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->configure == NULL || session == NULL || config == NULL ||
        config->sample_rate_hz == 0u || config->channels == 0u ||
        config->codec_config == NULL || config->codec_config_size == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->configure(decoder->user, session, config);
}

static inline h2_pal_result_t h2_pal_audio_decoder_submit_packet(
    const h2_pal_audio_decoder_api_t *decoder,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_packet_t *packet) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->submit_packet == NULL || session == NULL || packet == NULL ||
        (packet->flags & ~H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u ||
        packet->duration_us < 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const int is_eos =
        (packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u;
    if ((is_eos && (packet->data != NULL || packet->size != 0u)) ||
        (!is_eos && (packet->data == NULL || packet->size == 0u))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->submit_packet(decoder->user, session, packet);
}

static inline h2_pal_result_t h2_pal_audio_decoder_acquire_frame(
    const h2_pal_audio_decoder_api_t *decoder,
    h2_pal_audio_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    if (out_frame == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_frame = NULL;
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->acquire_frame == NULL || session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->acquire_frame(
        decoder->user, session, timeout_ms, out_frame);
}

static inline h2_pal_result_t h2_pal_audio_decoder_frame_get_info(
    const h2_pal_audio_decoder_api_t *decoder,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame,
    h2_audio_decoder_frame_info_t *out_info) {
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->frame_get_info == NULL || session == NULL || frame == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->frame_get_info(
        decoder->user, session, frame, out_info);
}

static inline h2_pal_result_t h2_pal_audio_decoder_release_frame(
    const h2_pal_audio_decoder_api_t *decoder,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->release_frame == NULL || session == NULL || frame == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->release_frame(decoder->user, session, frame);
}

static inline h2_pal_result_t h2_pal_audio_decoder_reset(
    const h2_pal_audio_decoder_api_t *decoder,
    h2_pal_audio_decoder_session_t *session) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->reset == NULL || session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->reset(decoder->user, session);
}

static inline h2_pal_result_t h2_pal_audio_decoder_close(
    const h2_pal_audio_decoder_api_t *decoder,
    h2_pal_audio_decoder_session_t *session) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->close == NULL || session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->close(decoder->user, session);
}

#ifdef __cplusplus
}
#endif

#endif
