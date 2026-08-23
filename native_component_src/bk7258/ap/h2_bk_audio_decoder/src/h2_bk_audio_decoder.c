#include "h2_bk_audio_decoder.h"

#include <common/bk_include.h>

#include "audio_osi_wrapper.h"
#include "modules/aacdec.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct h2_bk_audio_decoder_state {
    h2_pal_mem_api_t allocator;
    HAACDecoder decoder;
    int16_t *pcm;
    size_t pcm_capacity;
    h2_audio_decoder_frame_info_t frame_info;
    uint32_t sample_rate_hz;
    uint8_t channels;
    int configured;
    int ready;
    int acquired;
    int eos;
} h2_bk_audio_decoder_state_t;

struct h2_pal_audio_decoder_frame {
    h2_bk_audio_decoder_state_t *owner;
};

struct h2_pal_audio_decoder_session {
    h2_bk_audio_decoder_state_t state;
    struct h2_pal_audio_decoder_frame frame;
};

static int parse_audio_specific_config(
    const uint8_t *config,
    size_t size,
    uint32_t *out_sample_rate_hz,
    uint8_t *out_channels) {
    static const uint32_t sample_rates[] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
        22050u, 16000u, 12000u, 11025u, 8000u, 7350u,
    };
    if (config == NULL || size < 2u) {
        return 0;
    }
    const uint16_t bits = ((uint16_t)config[0] << 8u) | config[1];
    const uint8_t object_type = (uint8_t)((bits >> 11u) & 0x1fu);
    const uint8_t sample_rate_index = (uint8_t)((bits >> 7u) & 0x0fu);
    const uint8_t channels = (uint8_t)((bits >> 3u) & 0x0fu);
    if (object_type != 2u ||
        sample_rate_index >= sizeof(sample_rates) / sizeof(sample_rates[0]) ||
        channels == 0u || channels > AAC_MAX_NCHANS) {
        return 0;
    }
    *out_sample_rate_hz = sample_rates[sample_rate_index];
    *out_channels = channels;
    return 1;
}

static h2_pal_result_t map_decode_error(int error) {
    switch (error) {
    case ERR_AAC_NONE:
        return H2_PAL_OK;
    case ERR_AAC_NULL_POINTER:
        return H2_PAL_ERR_INVALID_ARG;
    case ERR_AAC_MPEG4_UNSUPPORTED:
    case ERR_AAC_NCHANS_TOO_HIGH:
    case ERR_AAC_SBR_NCHANS_TOO_HIGH:
        return H2_PAL_ERR_UNSUPPORTED;
    case ERR_AAC_INDATA_UNDERFLOW:
    case ERR_AAC_INVALID_ADTS_HEADER:
    case ERR_AAC_INVALID_ADIF_HEADER:
    case ERR_AAC_INVALID_FRAME:
    case ERR_AAC_RAWBLOCK_PARAMS:
        return H2_PAL_ERR_FORMAT;
    default:
        return H2_PAL_ERR_IO;
    }
}

static void release_configuration(h2_bk_audio_decoder_state_t *state) {
    if (state->decoder != NULL) {
        AACFreeDecoder(state->decoder);
        state->decoder = NULL;
    }
    h2_pal_mem_free(&state->allocator, state->pcm);
    state->pcm = NULL;
    state->pcm_capacity = 0u;
    state->configured = 0;
    state->ready = 0;
    state->eos = 0;
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
    if (session == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(*session));
    session->state.allocator = *config->pcm_allocator;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_configure(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_stream_config_t *config) {
    (void)user;
    h2_bk_audio_decoder_state_t *state = &session->state;
    if (state->configured || state->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->codec != H2_AUDIO_CODEC_AAC_LC ||
        config->bitstream_format != H2_AUDIO_BITSTREAM_AAC_RAW ||
        config->channels > AAC_MAX_NCHANS) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    uint32_t asc_sample_rate_hz = 0u;
    uint8_t asc_channels = 0u;
    if (!parse_audio_specific_config(
            config->codec_config,
            config->codec_config_size,
            &asc_sample_rate_hz,
            &asc_channels) ||
        asc_sample_rate_hz != config->sample_rate_hz ||
        asc_channels != config->channels) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t pcm_capacity =
        AAC_MAX_NSAMPS * config->channels * sizeof(int16_t);
    int16_t *pcm = h2_pal_mem_alloc(&state->allocator, pcm_capacity);
    if (pcm == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (bk_audio_osi_funcs_init() != BK_OK) {
        h2_pal_mem_free(&state->allocator, pcm);
        return H2_PAL_ERR_IO;
    }
    HAACDecoder decoder = AACInitDecoder();
    if (decoder == NULL) {
        h2_pal_mem_free(&state->allocator, pcm);
        return H2_PAL_ERR_NO_MEMORY;
    }
    AACFrameInfo frame_info = {
        .nChans = config->channels,
        .sampRateCore = (int)config->sample_rate_hz,
        .sampRateOut = (int)config->sample_rate_hz,
        .bitsPerSample = 16,
        .profile = AAC_PROFILE_LC,
    };
    const int raw_result = AACSetRawBlockParams(decoder, 0, &frame_info);
    if (raw_result != ERR_AAC_NONE) {
        AACFreeDecoder(decoder);
        h2_pal_mem_free(&state->allocator, pcm);
        return map_decode_error(raw_result);
    }
    state->decoder = decoder;
    state->pcm = pcm;
    state->pcm_capacity = pcm_capacity;
    state->sample_rate_hz = config->sample_rate_hz;
    state->channels = config->channels;
    state->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_submit(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_packet_t *packet) {
    (void)user;
    h2_bk_audio_decoder_state_t *state = &session->state;
    if (!state->configured || state->eos) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if ((packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u) {
        state->eos = 1;
        return H2_PAL_OK;
    }
    if (state->ready || state->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (packet->size > INT_MAX) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    unsigned char *input = (unsigned char *)(uintptr_t)packet->data;
    int bytes_left = (int)packet->size;
    const int decode_result =
        AACDecode(state->decoder, &input, &bytes_left, state->pcm);
    if (decode_result != ERR_AAC_NONE) {
        return map_decode_error(decode_result);
    }
    AACFrameInfo decoded = {0};
    AACGetLastFrameInfo(state->decoder, &decoded);
    if (bytes_left != 0 || decoded.nChans != state->channels ||
        decoded.sampRateOut != (int)state->sample_rate_hz ||
        decoded.bitsPerSample != 16 || decoded.outputSamps <= 0 ||
        decoded.outputSamps % state->channels != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t bytes = (size_t)decoded.outputSamps * sizeof(int16_t);
    if (bytes > state->pcm_capacity) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    state->frame_info = (h2_audio_decoder_frame_info_t){
        .data = state->pcm,
        .bytes = bytes,
        .sample_rate_hz = state->sample_rate_hz,
        .samples_per_channel =
            (uint32_t)(decoded.outputSamps / state->channels),
        .channels = state->channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .pts_us = packet->pts_us,
        .duration_us = packet->duration_us,
    };
    state->ready = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_acquire(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    (void)user;
    (void)timeout_ms;
    h2_bk_audio_decoder_state_t *state = &session->state;
    if (state->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (!state->ready) {
        return state->eos ? H2_PAL_EXIT : H2_PAL_ERR_WOULD_BLOCK;
    }
    session->frame.owner = state;
    state->acquired = 1;
    *out_frame = &session->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_get_info(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame,
    h2_audio_decoder_frame_info_t *out_info) {
    (void)user;
    h2_bk_audio_decoder_state_t *state = &session->state;
    if (!state->acquired || frame != &session->frame ||
        frame->owner != state) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = state->frame_info;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_release(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    h2_pal_audio_decoder_frame_t *frame) {
    (void)user;
    h2_bk_audio_decoder_state_t *state = &session->state;
    if (!state->acquired || frame != &session->frame ||
        frame->owner != state) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    state->acquired = 0;
    state->ready = 0;
    session->frame.owner = NULL;
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_reset(
    void *user,
    h2_pal_audio_decoder_session_t *session) {
    (void)user;
    if (session->state.acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    release_configuration(&session->state);
    return H2_PAL_OK;
}

static h2_pal_result_t decoder_close(
    void *user,
    h2_pal_audio_decoder_session_t *session) {
    (void)user;
    if (session->state.acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const h2_pal_mem_api_t allocator = session->state.allocator;
    release_configuration(&session->state);
    h2_pal_mem_free(&allocator, session);
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

const h2_pal_audio_decoder_api_t *h2_bk_audio_decoder_api(void) {
    return &s_api;
}
