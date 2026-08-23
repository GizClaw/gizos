#include "h2_ffmpeg.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

struct h2_pal_audio_decoder_session;

struct h2_pal_audio_decoder_frame {
    struct h2_pal_audio_decoder_session *owner;
};

struct h2_pal_audio_decoder_session {
    h2_pal_mem_api_t allocator;
    AVCodecContext *codec;
    AVFrame *decoded;
    uint8_t *input;
    size_t input_capacity;
    uint8_t *output;
    size_t output_capacity;
    h2_audio_decoder_frame_info_t frame_info;
    struct h2_pal_audio_decoder_frame frame;
    uint32_t sample_rate_hz;
    uint8_t channels;
    int configured;
    int eos_submitted;
    int acquired;
};

static h2_pal_result_t audio_ffmpeg_result(int result) {
    if (result == AVERROR(EAGAIN)) return H2_PAL_ERR_WOULD_BLOCK;
    if (result == AVERROR_EOF) return H2_PAL_EXIT;
    if (result == AVERROR(ENOMEM)) return H2_PAL_ERR_NO_MEMORY;
    if (result == AVERROR_INVALIDDATA) return H2_PAL_ERR_FORMAT;
    return H2_PAL_ERR_IO;
}

static void audio_release_codec(
    struct h2_pal_audio_decoder_session *session) {
    av_frame_free(&session->decoded);
    avcodec_free_context(&session->codec);
    session->configured = 0;
    session->eos_submitted = 0;
    memset(&session->frame_info, 0, sizeof(session->frame_info));
}

static h2_pal_result_t audio_reserve_output(
    struct h2_pal_audio_decoder_session *session,
    size_t required) {
    if (required <= session->output_capacity) return H2_PAL_OK;
    uint8_t *replacement = h2_pal_mem_alloc(&session->allocator, required);
    if (replacement == NULL) return H2_PAL_ERR_NO_MEMORY;
    h2_pal_mem_free(&session->allocator, session->output);
    session->output = replacement;
    session->output_capacity = required;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_reserve_input(
    struct h2_pal_audio_decoder_session *session,
    size_t packet_size) {
    if (packet_size > SIZE_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const size_t required = packet_size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (required > session->input_capacity) {
        uint8_t *replacement =
            h2_pal_mem_alloc(&session->allocator, required);
        if (replacement == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        h2_pal_mem_free(&session->allocator, session->input);
        session->input = replacement;
        session->input_capacity = required;
    }
    memset(
        session->input + packet_size,
        0,
        AV_INPUT_BUFFER_PADDING_SIZE);
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_audio_open(
    void *user,
    const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
    (void)user;
    if (config->preferred_format != H2_AUDIO_SAMPLE_S16LE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    struct h2_pal_audio_decoder_session *session =
        h2_pal_mem_alloc(config->pcm_allocator, sizeof(*session));
    if (session == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(session, 0, sizeof(*session));
    session->allocator = *config->pcm_allocator;
    session->frame.owner = session;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_audio_configure(
    void *user,
    h2_pal_audio_decoder_session_t *opaque_session,
    const h2_audio_decoder_stream_config_t *config) {
    (void)user;
    struct h2_pal_audio_decoder_session *session = opaque_session;
    if (session->configured || session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->codec != H2_AUDIO_CODEC_AAC_LC ||
        config->bitstream_format != H2_AUDIO_BITSTREAM_AAC_RAW) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (config->codec_config_size > (size_t)INT_MAX ||
        config->sample_rate_hz > (uint32_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec == NULL) return H2_PAL_ERR_UNSUPPORTED;
    session->codec = avcodec_alloc_context3(codec);
    session->decoded = av_frame_alloc();
    if (session->codec == NULL || session->decoded == NULL) {
        audio_release_codec(session);
        return H2_PAL_ERR_NO_MEMORY;
    }
    session->codec->sample_rate = (int)config->sample_rate_hz;
    av_channel_layout_default(&session->codec->ch_layout, config->channels);
    session->codec->pkt_timebase = (AVRational){1, 1000000};
    session->codec->extradata =
        av_mallocz(config->codec_config_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (session->codec->extradata == NULL) {
        audio_release_codec(session);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(
        session->codec->extradata,
        config->codec_config,
        config->codec_config_size);
    session->codec->extradata_size = (int)config->codec_config_size;
    const int open_result = avcodec_open2(session->codec, codec, NULL);
    if (open_result < 0) {
        audio_release_codec(session);
        return audio_ffmpeg_result(open_result);
    }
    session->sample_rate_hz = config->sample_rate_hz;
    session->channels = config->channels;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_audio_submit_packet(
    void *user,
    h2_pal_audio_decoder_session_t *opaque_session,
    const h2_audio_decoder_packet_t *packet) {
    (void)user;
    struct h2_pal_audio_decoder_session *session = opaque_session;
    if (!session->configured || session->eos_submitted) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->acquired) return H2_PAL_ERR_WOULD_BLOCK;

    int result;
    if ((packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u) {
        result = avcodec_send_packet(session->codec, NULL);
        if (result == 0) session->eos_submitted = 1;
    } else {
        if (packet->size > (size_t)INT_MAX) return H2_PAL_ERR_INVALID_ARG;
        h2_pal_result_t reserve_result =
            audio_reserve_input(session, packet->size);
        if (reserve_result != H2_PAL_OK) {
            return reserve_result;
        }
        memcpy(session->input, packet->data, packet->size);
        AVPacket av_packet = {0};
        av_packet.data = session->input;
        av_packet.size = (int)packet->size;
        av_packet.pts = packet->pts_us;
        av_packet.dts = packet->dts_us;
        av_packet.duration = packet->duration_us;
        result = avcodec_send_packet(session->codec, &av_packet);
    }
    return result == 0 ? H2_PAL_OK : audio_ffmpeg_result(result);
}

/*
 * Keep sample conversion free of libm so static PAL consumers do not inherit
 * a platform-specific link dependency.
 */
static int16_t sample_from_float(float value) {
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    const float scaled = value * 32767.0f;
    return (int16_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

static int16_t sample_from_double(double value) {
    if (value > 1.0) value = 1.0;
    if (value < -1.0) value = -1.0;
    const double scaled = value * 32767.0;
    return (int16_t)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
}

static h2_pal_result_t convert_audio_frame(
    struct h2_pal_audio_decoder_session *session) {
    const int channels = session->decoded->ch_layout.nb_channels;
    const int samples = session->decoded->nb_samples;
    if (channels <= 0 || channels > UINT8_MAX || samples <= 0 ||
        channels != session->channels) {
        return H2_PAL_ERR_FORMAT;
    }
    if ((size_t)samples > SIZE_MAX / (size_t)channels ||
        (size_t)samples * (size_t)channels > SIZE_MAX / sizeof(int16_t)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const size_t sample_count = (size_t)samples * (size_t)channels;
    const size_t required = sample_count * sizeof(int16_t);
    h2_pal_result_t result = audio_reserve_output(session, required);
    if (result != H2_PAL_OK) return result;
    int16_t *destination = (int16_t *)session->output;
    const enum AVSampleFormat format =
        (enum AVSampleFormat)session->decoded->format;
    const int planar = av_sample_fmt_is_planar(format);
    const enum AVSampleFormat packed = av_get_packed_sample_fmt(format);
    if (session->decoded->extended_data == NULL) return H2_PAL_ERR_FORMAT;

    for (int sample = 0; sample < samples; ++sample) {
        for (int channel = 0; channel < channels; ++channel) {
            const int source_channel = planar ? channel : 0;
            const size_t source_index =
                planar ? (size_t)sample :
                    (size_t)sample * (size_t)channels + (size_t)channel;
            const uint8_t *source = session->decoded->extended_data[source_channel];
            if (source == NULL) return H2_PAL_ERR_FORMAT;
            int16_t converted;
            switch (packed) {
            case AV_SAMPLE_FMT_S16:
                converted = ((const int16_t *)source)[source_index];
                break;
            case AV_SAMPLE_FMT_FLT:
                converted = sample_from_float(
                    ((const float *)source)[source_index]);
                break;
            case AV_SAMPLE_FMT_DBL:
                converted = sample_from_double(
                    ((const double *)source)[source_index]);
                break;
            default:
                return H2_PAL_ERR_UNSUPPORTED;
            }
            destination[(size_t)sample * (size_t)channels + (size_t)channel] =
                converted;
        }
    }
    session->frame_info = (h2_audio_decoder_frame_info_t){
        .data = session->output,
        .bytes = required,
        .sample_rate_hz = (uint32_t)session->decoded->sample_rate,
        .samples_per_channel = (uint32_t)samples,
        .channels = (uint8_t)channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .pts_us =
            session->decoded->pts == AV_NOPTS_VALUE ? 0 : session->decoded->pts,
        .duration_us = session->decoded->duration,
    };
    if (session->frame_info.duration_us == 0 &&
        session->frame_info.sample_rate_hz != 0u) {
        session->frame_info.duration_us =
            ((int64_t)samples * INT64_C(1000000)) /
            session->frame_info.sample_rate_hz;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_audio_acquire_frame(
    void *user,
    h2_pal_audio_decoder_session_t *opaque_session,
    uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    (void)user;
    struct h2_pal_audio_decoder_session *session = opaque_session;
    if (!session->configured) return H2_PAL_ERR_INVALID_STATE;
    if (session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    const int receive_result = avcodec_receive_frame(session->codec, session->decoded);
    if (receive_result == AVERROR(EAGAIN)) {
        return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
    }
    if (receive_result < 0) return audio_ffmpeg_result(receive_result);
    h2_pal_result_t result = convert_audio_frame(session);
    if (result != H2_PAL_OK) {
        av_frame_unref(session->decoded);
        return result;
    }
    session->acquired = 1;
    *out_frame = &session->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_audio_frame_get_info(
    void *user,
    h2_pal_audio_decoder_session_t *opaque_session,
    h2_pal_audio_decoder_frame_t *opaque_frame,
    h2_audio_decoder_frame_info_t *out_info) {
    (void)user;
    struct h2_pal_audio_decoder_session *session = opaque_session;
    struct h2_pal_audio_decoder_frame *frame = opaque_frame;
    if (!session->acquired || frame != &session->frame ||
        frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = session->frame_info;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_audio_release_frame(
    void *user,
    h2_pal_audio_decoder_session_t *opaque_session,
    h2_pal_audio_decoder_frame_t *opaque_frame) {
    (void)user;
    struct h2_pal_audio_decoder_session *session = opaque_session;
    struct h2_pal_audio_decoder_frame *frame = opaque_frame;
    if (!session->acquired || frame != &session->frame ||
        frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    av_frame_unref(session->decoded);
    session->acquired = 0;
    memset(&session->frame_info, 0, sizeof(session->frame_info));
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_audio_reset(
    void *user,
    h2_pal_audio_decoder_session_t *opaque_session) {
    (void)user;
    struct h2_pal_audio_decoder_session *session = opaque_session;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    audio_release_codec(session);
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_audio_close(
    void *user,
    h2_pal_audio_decoder_session_t *opaque_session) {
    (void)user;
    struct h2_pal_audio_decoder_session *session = opaque_session;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    audio_release_codec(session);
    h2_pal_mem_free(&session->allocator, session->input);
    h2_pal_mem_free(&session->allocator, session->output);
    h2_pal_mem_api_t allocator = session->allocator;
    memset(session, 0, sizeof(*session));
    h2_pal_mem_free(&allocator, session);
    return H2_PAL_OK;
}

static const h2_pal_audio_decoder_vtable_t s_desktop_audio_decoder_vtable = {
    .open = desktop_audio_open,
    .configure = desktop_audio_configure,
    .submit_packet = desktop_audio_submit_packet,
    .acquire_frame = desktop_audio_acquire_frame,
    .frame_get_info = desktop_audio_frame_get_info,
    .release_frame = desktop_audio_release_frame,
    .reset = desktop_audio_reset,
    .close = desktop_audio_close,
};

static const h2_pal_audio_decoder_api_t s_desktop_audio_decoder_api = {
    .user = NULL,
    .vtable = &s_desktop_audio_decoder_vtable,
};

const h2_pal_audio_decoder_api_t *h2_ffmpeg_audio_decoder_api(void) {
    return &s_desktop_audio_decoder_api;
}
