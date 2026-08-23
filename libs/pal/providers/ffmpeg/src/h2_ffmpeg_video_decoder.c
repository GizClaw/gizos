#include "h2_ffmpeg.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

struct h2_pal_video_decoder_session;

struct h2_pal_video_decoder_frame {
    struct h2_pal_video_decoder_session *owner;
};

struct h2_pal_video_decoder_session {
    h2_pal_mem_api_t allocator;
    h2_video_pixel_format_t output_format;
    uint32_t visible_width;
    uint32_t visible_height;
    AVCodecContext *codec;
    AVFrame *decoded;
    struct SwsContext *scaler;
    uint8_t *input;
    size_t input_capacity;
    uint8_t *output;
    size_t output_capacity;
    h2_video_frame_info_t frame_info;
    struct h2_pal_video_decoder_frame frame;
    int configured;
    int eos_submitted;
    int acquired;
};

static h2_pal_result_t ffmpeg_result(int result) {
    if (result == AVERROR(EAGAIN)) return H2_PAL_ERR_WOULD_BLOCK;
    if (result == AVERROR_EOF) return H2_PAL_EXIT;
    if (result == AVERROR(ENOMEM)) return H2_PAL_ERR_NO_MEMORY;
    if (result == AVERROR_INVALIDDATA) return H2_PAL_ERR_FORMAT;
    return H2_PAL_ERR_IO;
}

static int checked_mul_size(size_t left, size_t right, size_t *out_value) {
    if (out_value == NULL || (right != 0u && left > SIZE_MAX / right)) {
        return 0;
    }
    *out_value = left * right;
    return 1;
}

static h2_pal_result_t reserve_output(
    struct h2_pal_video_decoder_session *session,
    size_t required) {
    if (required <= session->output_capacity) return H2_PAL_OK;
    uint8_t *replacement = h2_pal_mem_alloc(&session->allocator, required);
    if (replacement == NULL) return H2_PAL_ERR_NO_MEMORY;
    h2_pal_mem_free(&session->allocator, session->output);
    session->output = replacement;
    session->output_capacity = required;
    return H2_PAL_OK;
}

static h2_pal_result_t reserve_input(
    struct h2_pal_video_decoder_session *session,
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

static void release_codec(struct h2_pal_video_decoder_session *session) {
    sws_freeContext(session->scaler);
    session->scaler = NULL;
    av_frame_free(&session->decoded);
    avcodec_free_context(&session->codec);
    session->configured = 0;
    session->eos_submitted = 0;
    memset(&session->frame_info, 0, sizeof(session->frame_info));
}

static h2_pal_result_t desktop_open(
    void *user,
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session) {
    (void)user;
    if (config->preferred_format != H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED &&
        config->preferred_format != H2_VIDEO_PIXEL_FORMAT_YUV420P &&
        config->preferred_format != H2_VIDEO_PIXEL_FORMAT_RGB565) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    struct h2_pal_video_decoder_session *session =
        h2_pal_mem_alloc(config->frame_allocator, sizeof(*session));
    if (session == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(session, 0, sizeof(*session));
    session->allocator = *config->frame_allocator;
    session->output_format =
        config->preferred_format == H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED
            ? H2_VIDEO_PIXEL_FORMAT_RGB565
            : config->preferred_format;
    session->frame.owner = session;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_configure(
    void *user,
    h2_pal_video_decoder_session_t *opaque_session,
    const h2_video_decoder_stream_config_t *config) {
    (void)user;
    struct h2_pal_video_decoder_session *session = opaque_session;
    if (session->configured || session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->codec != H2_VIDEO_CODEC_H264 ||
        config->bitstream_format != H2_VIDEO_BITSTREAM_H264_ANNEX_B) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (config->codec_config_size > (size_t)INT_MAX ||
        config->coded_width > (uint32_t)INT_MAX ||
        config->coded_height > (uint32_t)INT_MAX ||
        config->visible_width > (uint32_t)(INT_MAX / (int)sizeof(uint16_t)) ||
        config->visible_height > (uint32_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == NULL) return H2_PAL_ERR_UNSUPPORTED;
    session->codec = avcodec_alloc_context3(codec);
    session->decoded = av_frame_alloc();
    if (session->codec == NULL || session->decoded == NULL) {
        release_codec(session);
        return H2_PAL_ERR_NO_MEMORY;
    }
    session->codec->width = (int)config->coded_width;
    session->codec->height = (int)config->coded_height;
    session->codec->pkt_timebase = (AVRational){1, 1000000};
    session->codec->thread_count = 1;
    session->codec->extradata =
        av_mallocz(config->codec_config_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (session->codec->extradata == NULL) {
        release_codec(session);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(
        session->codec->extradata,
        config->codec_config,
        config->codec_config_size);
    session->codec->extradata_size = (int)config->codec_config_size;
    const int open_result = avcodec_open2(session->codec, codec, NULL);
    if (open_result < 0) {
        release_codec(session);
        return ffmpeg_result(open_result);
    }
    session->visible_width = config->visible_width;
    session->visible_height = config->visible_height;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_submit_packet(
    void *user,
    h2_pal_video_decoder_session_t *opaque_session,
    const h2_video_decoder_packet_t *packet) {
    (void)user;
    struct h2_pal_video_decoder_session *session = opaque_session;
    if (!session->configured || session->eos_submitted) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (session->acquired) return H2_PAL_ERR_WOULD_BLOCK;

    int result;
    if ((packet->flags & H2_VIDEO_DECODER_PACKET_END_OF_STREAM) != 0u) {
        result = avcodec_send_packet(session->codec, NULL);
        if (result == 0) session->eos_submitted = 1;
    } else {
        if (packet->size > (size_t)INT_MAX) return H2_PAL_ERR_INVALID_ARG;
        h2_pal_result_t reserve_result =
            reserve_input(session, packet->size);
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
    return result == 0 ? H2_PAL_OK : ffmpeg_result(result);
}

static h2_pal_result_t copy_yuv420p(
    struct h2_pal_video_decoder_session *session) {
    if (session->decoded->format != AV_PIX_FMT_YUV420P) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (session->decoded->width < 0 || session->decoded->height < 0 ||
        (uint32_t)session->decoded->width < session->visible_width ||
        (uint32_t)session->decoded->height < session->visible_height) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t width = session->visible_width;
    const size_t height = session->visible_height;
    const size_t chroma_width = (width + 1u) / 2u;
    const size_t chroma_height = (height + 1u) / 2u;
    size_t y_bytes;
    size_t chroma_bytes;
    if (!checked_mul_size(width, height, &y_bytes) ||
        !checked_mul_size(chroma_width, chroma_height, &chroma_bytes) ||
        chroma_bytes > (SIZE_MAX - y_bytes) / 2u) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const size_t required = y_bytes + chroma_bytes * 2u;
    h2_pal_result_t result = reserve_output(session, required);
    if (result != H2_PAL_OK) return result;

    uint8_t *planes[3] = {
        session->output,
        session->output + y_bytes,
        session->output + y_bytes + chroma_bytes,
    };
    const size_t rows[3] = {height, chroma_height, chroma_height};
    const size_t row_bytes[3] = {width, chroma_width, chroma_width};
    for (size_t plane = 0u; plane < 3u; ++plane) {
        if (session->decoded->data[plane] == NULL ||
            session->decoded->linesize[plane] < 0 ||
            (size_t)session->decoded->linesize[plane] < row_bytes[plane]) {
            return H2_PAL_ERR_FORMAT;
        }
        for (size_t row = 0u; row < rows[plane]; ++row) {
            memcpy(
                planes[plane] + row * row_bytes[plane],
                session->decoded->data[plane] +
                    row * (size_t)session->decoded->linesize[plane],
                row_bytes[plane]);
        }
    }
    session->frame_info.planes[0] = (h2_video_frame_plane_t){
        .data = planes[0], .bytes = y_bytes, .stride_bytes = width};
    session->frame_info.planes[1] = (h2_video_frame_plane_t){
        .data = planes[1], .bytes = chroma_bytes, .stride_bytes = chroma_width};
    session->frame_info.planes[2] = (h2_video_frame_plane_t){
        .data = planes[2], .bytes = chroma_bytes, .stride_bytes = chroma_width};
    session->frame_info.plane_count = 3u;
    return H2_PAL_OK;
}

static h2_pal_result_t convert_rgb565(
    struct h2_pal_video_decoder_session *session) {
    size_t pixels;
    size_t required;
    if (!checked_mul_size(
            session->visible_width, session->visible_height, &pixels) ||
        !checked_mul_size(pixels, sizeof(uint16_t), &required)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_pal_result_t result = reserve_output(session, required);
    if (result != H2_PAL_OK) return result;

    session->scaler = sws_getCachedContext(
        session->scaler,
        session->decoded->width,
        session->decoded->height,
        (enum AVPixelFormat)session->decoded->format,
        (int)session->visible_width,
        (int)session->visible_height,
        AV_PIX_FMT_RGB565LE,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL);
    if (session->scaler == NULL) return H2_PAL_ERR_NO_MEMORY;
    uint8_t *destination[4] = {session->output, NULL, NULL, NULL};
    int destination_stride[4] = {
        (int)(session->visible_width * sizeof(uint16_t)), 0, 0, 0};
    const int rows = sws_scale(
        session->scaler,
        (const uint8_t *const *)session->decoded->data,
        session->decoded->linesize,
        0,
        session->decoded->height,
        destination,
        destination_stride);
    if (rows != (int)session->visible_height) return H2_PAL_ERR_FORMAT;
    session->frame_info.planes[0] = (h2_video_frame_plane_t){
        .data = session->output,
        .bytes = required,
        .stride_bytes = session->visible_width * sizeof(uint16_t),
    };
    session->frame_info.plane_count = 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_acquire_frame(
    void *user,
    h2_pal_video_decoder_session_t *opaque_session,
    uint32_t timeout_ms,
    h2_pal_video_decoder_frame_t **out_frame) {
    (void)user;
    struct h2_pal_video_decoder_session *session = opaque_session;
    if (!session->configured) return H2_PAL_ERR_INVALID_STATE;
    if (session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    const int receive_result = avcodec_receive_frame(session->codec, session->decoded);
    if (receive_result == AVERROR(EAGAIN)) {
        return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
    }
    if (receive_result < 0) return ffmpeg_result(receive_result);

    memset(&session->frame_info, 0, sizeof(session->frame_info));
    session->frame_info.format = session->output_format;
    session->frame_info.width = session->visible_width;
    session->frame_info.height = session->visible_height;
    session->frame_info.pts_us =
        session->decoded->pts == AV_NOPTS_VALUE ? 0 : session->decoded->pts;
    session->frame_info.duration_us = session->decoded->duration;
    h2_pal_result_t result =
        session->output_format == H2_VIDEO_PIXEL_FORMAT_YUV420P
            ? copy_yuv420p(session)
            : convert_rgb565(session);
    if (result != H2_PAL_OK) {
        av_frame_unref(session->decoded);
        return result;
    }
    session->acquired = 1;
    *out_frame = &session->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_frame_get_info(
    void *user,
    h2_pal_video_decoder_session_t *opaque_session,
    h2_pal_video_decoder_frame_t *opaque_frame,
    h2_video_frame_info_t *out_info) {
    (void)user;
    struct h2_pal_video_decoder_session *session = opaque_session;
    struct h2_pal_video_decoder_frame *frame = opaque_frame;
    if (!session->acquired || frame->owner != session || frame != &session->frame) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_info = session->frame_info;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_release_frame(
    void *user,
    h2_pal_video_decoder_session_t *opaque_session,
    h2_pal_video_decoder_frame_t *opaque_frame) {
    (void)user;
    struct h2_pal_video_decoder_session *session = opaque_session;
    struct h2_pal_video_decoder_frame *frame = opaque_frame;
    if (!session->acquired || frame->owner != session || frame != &session->frame) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    av_frame_unref(session->decoded);
    session->acquired = 0;
    memset(&session->frame_info, 0, sizeof(session->frame_info));
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_reset(
    void *user,
    h2_pal_video_decoder_session_t *opaque_session) {
    (void)user;
    struct h2_pal_video_decoder_session *session = opaque_session;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    release_codec(session);
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_close(
    void *user,
    h2_pal_video_decoder_session_t *opaque_session) {
    (void)user;
    struct h2_pal_video_decoder_session *session = opaque_session;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    release_codec(session);
    h2_pal_mem_free(&session->allocator, session->input);
    h2_pal_mem_free(&session->allocator, session->output);
    h2_pal_mem_api_t allocator = session->allocator;
    memset(session, 0, sizeof(*session));
    h2_pal_mem_free(&allocator, session);
    return H2_PAL_OK;
}

static const h2_pal_video_decoder_vtable_t s_desktop_video_decoder_vtable = {
    .open = desktop_open,
    .configure = desktop_configure,
    .submit_packet = desktop_submit_packet,
    .acquire_frame = desktop_acquire_frame,
    .frame_get_info = desktop_frame_get_info,
    .release_frame = desktop_release_frame,
    .reset = desktop_reset,
    .close = desktop_close,
};

static const h2_pal_video_decoder_api_t s_desktop_video_decoder_api = {
    .user = NULL,
    .vtable = &s_desktop_video_decoder_vtable,
};

const h2_pal_video_decoder_api_t *h2_ffmpeg_video_decoder_api(void) {
    return &s_desktop_video_decoder_api;
}
