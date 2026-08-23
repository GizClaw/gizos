#include "h2_linux_fdk_aac_decoder.h"

#include "aacdecoder_lib.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define H2_FDK_AAC_MAX_CHANNELS 2u
#define H2_FDK_AAC_MAX_SAMPLES_PER_CHANNEL 4096u
#define H2_FDK_AAC_PCM_VALUES \
    (H2_FDK_AAC_MAX_CHANNELS * H2_FDK_AAC_MAX_SAMPLES_PER_CHANNEL)

typedef struct fdk_aac_session {
    h2_pal_mem_api_t allocator;
    HANDLE_AACDECODER decoder;
    uint8_t *packet;
    size_t packet_capacity;
    size_t packet_size;
    INT_PCM *pcm;
    h2_audio_decoder_frame_info_t frame_info;
    int64_t packet_pts_us;
    uint32_t sample_rate_hz;
    uint8_t channels;
    int configured;
    int ready;
    int acquired;
    int eos;
} fdk_aac_session_t;

struct h2_pal_audio_decoder_frame {
    fdk_aac_session_t *owner;
};

struct h2_pal_audio_decoder_session {
    fdk_aac_session_t state;
    struct h2_pal_audio_decoder_frame frame;
};

static h2_pal_result_t map_fdk_error(AAC_DECODER_ERROR error) {
    switch (error) {
    case AAC_DEC_OK:
        return H2_PAL_OK;
    case AAC_DEC_OUT_OF_MEMORY:
        return H2_PAL_ERR_NO_MEMORY;
    case AAC_DEC_UNSUPPORTED_AOT:
    case AAC_DEC_UNSUPPORTED_FORMAT:
    case AAC_DEC_UNSUPPORTED_ER_FORMAT:
    case AAC_DEC_UNSUPPORTED_EPCONFIG:
    case AAC_DEC_UNSUPPORTED_MULTILAYER:
    case AAC_DEC_UNSUPPORTED_CHANNELCONFIG:
    case AAC_DEC_UNSUPPORTED_SAMPLINGRATE:
    case AAC_DEC_INVALID_SBR_CONFIG:
        return H2_PAL_ERR_UNSUPPORTED;
    case AAC_DEC_NOT_ENOUGH_BITS:
        return H2_PAL_ERR_WOULD_BLOCK;
    case AAC_DEC_OUTPUT_BUFFER_TOO_SMALL:
        return H2_PAL_ERR_NO_MEMORY;
    case AAC_DEC_INVALID_HANDLE:
    case AAC_DEC_SET_PARAM_FAIL:
        return H2_PAL_ERR_INVALID_ARG;
    default:
        return H2_PAL_ERR_FORMAT;
    }
}

static void release_configuration(fdk_aac_session_t *session) {
    if (session->decoder != NULL) {
        aacDecoder_Close(session->decoder);
        session->decoder = NULL;
    }
    h2_pal_mem_free(&session->allocator, session->packet);
    h2_pal_mem_free(&session->allocator, session->pcm);
    session->packet = NULL;
    session->packet_capacity = 0u;
    session->packet_size = 0u;
    session->pcm = NULL;
    session->configured = 0;
    session->ready = 0;
    session->eos = 0;
}

static h2_pal_result_t decoder_open(
    void *user,
    const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
    (void)user;
    if (config->preferred_format != H2_AUDIO_SAMPLE_S16LE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_audio_decoder_session_t *session =
        h2_pal_mem_alloc(config->pcm_allocator, sizeof(*session));
    if (session == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(session, 0, sizeof(*session));
    session->state.allocator = *config->pcm_allocator;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_configure(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    const h2_audio_decoder_stream_config_t *config) {
    (void)user;
    fdk_aac_session_t *session = &opaque->state;
    if (session->configured || session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->codec != H2_AUDIO_CODEC_AAC_LC ||
        config->bitstream_format != H2_AUDIO_BITSTREAM_AAC_RAW ||
        config->channels > H2_FDK_AAC_MAX_CHANNELS ||
        config->codec_config_size > UINT_MAX) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    HANDLE_AACDECODER decoder = aacDecoder_Open(TT_MP4_RAW, 1u);
    if (decoder == NULL) return H2_PAL_ERR_NO_MEMORY;
    UCHAR *raw_config = (UCHAR *)(uintptr_t)config->codec_config;
    const UINT raw_config_size = (UINT)config->codec_config_size;
    const AAC_DECODER_ERROR configure =
        aacDecoder_ConfigRaw(decoder, &raw_config, &raw_config_size);
    if (configure != AAC_DEC_OK) {
        aacDecoder_Close(decoder);
        return map_fdk_error(configure);
    }
    INT_PCM *pcm = h2_pal_mem_alloc(
        &session->allocator,
        H2_FDK_AAC_PCM_VALUES * sizeof(*pcm));
    if (pcm == NULL) {
        aacDecoder_Close(decoder);
        return H2_PAL_ERR_NO_MEMORY;
    }
    session->decoder = decoder;
    session->pcm = pcm;
    session->sample_rate_hz = config->sample_rate_hz;
    session->channels = config->channels;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t reserve_packet(
    fdk_aac_session_t *session,
    size_t required) {
    if (required <= session->packet_capacity) return H2_PAL_OK;
    uint8_t *packet = h2_pal_mem_alloc(&session->allocator, required);
    if (packet == NULL) return H2_PAL_ERR_NO_MEMORY;
    h2_pal_mem_free(&session->allocator, session->packet);
    session->packet = packet;
    session->packet_capacity = required;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_submit(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    const h2_audio_decoder_packet_t *packet) {
    (void)user;
    fdk_aac_session_t *session = &opaque->state;
    if (!session->configured || session->eos) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (packet->flags != 0u) {
        if (session->packet_size != 0u || session->ready || session->acquired) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        session->eos = 1;
        return H2_PAL_OK;
    }
    if (session->packet_size != 0u || session->ready || session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (packet->size > UINT_MAX) return H2_PAL_ERR_NO_MEMORY;
    h2_pal_result_t result = reserve_packet(session, packet->size);
    if (result != H2_PAL_OK) return result;
    memcpy(session->packet, packet->data, packet->size);
    session->packet_size = packet->size;
    session->packet_pts_us = packet->pts_us;
    return H2_PAL_OK;
}

static h2_pal_result_t decode_packet(fdk_aac_session_t *session) {
    UCHAR *input = session->packet;
    const UINT input_size = (UINT)session->packet_size;
    UINT input_valid = input_size;
    AAC_DECODER_ERROR error = aacDecoder_Fill(
        session->decoder, &input, &input_size, &input_valid);
    if (input_valid != 0u && input_valid < input_size) {
        memmove(
            session->packet,
            session->packet + input_size - input_valid,
            input_valid);
    }
    session->packet_size = input_valid;
    if (error != AAC_DEC_OK) return map_fdk_error(error);
    error = aacDecoder_DecodeFrame(
        session->decoder,
        session->pcm,
        (INT)H2_FDK_AAC_PCM_VALUES,
        0u);
    if (error == AAC_DEC_NOT_ENOUGH_BITS) return H2_PAL_ERR_WOULD_BLOCK;
    if (error != AAC_DEC_OK &&
        (error < aac_dec_decode_error_start ||
         error > aac_dec_decode_error_end)) {
        return map_fdk_error(error);
    }
    CStreamInfo *stream = aacDecoder_GetStreamInfo(session->decoder);
    if (stream == NULL || stream->sampleRate <= 0 || stream->frameSize <= 0 ||
        stream->numChannels <= 0 ||
        stream->numChannels > (INT)H2_FDK_AAC_MAX_CHANNELS ||
        stream->sampleRate != (INT)session->sample_rate_hz ||
        stream->numChannels != session->channels ||
        (uint64_t)stream->frameSize * (uint64_t)stream->numChannels >
            H2_FDK_AAC_PCM_VALUES) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t values =
        (size_t)stream->frameSize * (size_t)stream->numChannels;
    session->frame_info = (h2_audio_decoder_frame_info_t){
        .data = session->pcm,
        .bytes = values * sizeof(*session->pcm),
        .sample_rate_hz = (uint32_t)stream->sampleRate,
        .samples_per_channel = (uint32_t)stream->frameSize,
        .channels = (uint8_t)stream->numChannels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .pts_us = session->packet_pts_us,
        .duration_us =
            (int64_t)stream->frameSize * INT64_C(1000000) /
            stream->sampleRate,
    };
    session->ready = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_acquire(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    (void)user;
    (void)timeout_ms;
    fdk_aac_session_t *session = &opaque->state;
    if (session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    if (!session->ready && session->packet_size != 0u) {
        const h2_pal_result_t result = decode_packet(session);
        if (result != H2_PAL_OK) return result;
    }
    if (!session->ready) {
        return session->eos ? H2_PAL_EXIT : H2_PAL_ERR_WOULD_BLOCK;
    }
    opaque->frame.owner = session;
    session->acquired = 1;
    *out_frame = &opaque->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_get_info(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    h2_pal_audio_decoder_frame_t *frame,
    h2_audio_decoder_frame_info_t *out_info) {
    (void)user;
    fdk_aac_session_t *session = &opaque->state;
    if (!session->acquired || frame != &opaque->frame ||
        frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = session->frame_info;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_release(
    void *user,
    h2_pal_audio_decoder_session_t *opaque,
    h2_pal_audio_decoder_frame_t *frame) {
    (void)user;
    fdk_aac_session_t *session = &opaque->state;
    if (!session->acquired || frame != &opaque->frame ||
        frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    session->acquired = 0;
    session->ready = 0;
    opaque->frame.owner = NULL;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_reset(
    void *user,
    h2_pal_audio_decoder_session_t *opaque) {
    (void)user;
    fdk_aac_session_t *session = &opaque->state;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    release_configuration(session);
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_close(
    void *user,
    h2_pal_audio_decoder_session_t *opaque) {
    (void)user;
    fdk_aac_session_t *session = &opaque->state;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    const h2_pal_mem_api_t allocator = session->allocator;
    release_configuration(session);
    h2_pal_mem_free(&allocator, opaque);
    return H2_PAL_OK;
}

static const h2_pal_audio_decoder_vtable_t s_vtable = {
    .open = decoder_open,
    .configure = decoder_configure,
    .submit_packet = decoder_submit,
    .acquire_frame = decoder_acquire,
    .frame_get_info = decoder_get_info,
    .release_frame = decoder_release,
    .reset = decoder_reset,
    .close = decoder_close,
};

static const h2_pal_audio_decoder_api_t s_api = {
    .user = NULL,
    .vtable = &s_vtable,
};

const h2_pal_audio_decoder_api_t *h2_linux_fdk_aac_decoder_api(void) {
    return &s_api;
}
