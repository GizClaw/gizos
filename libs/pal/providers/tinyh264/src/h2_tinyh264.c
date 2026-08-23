#include "h2_tinyh264.h"

#include "h264bsd_decoder.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

const h2_pal_mem_api_t *h2_tinyh264_allocator_scope_enter(
    const h2_pal_mem_api_t *allocator);
void h2_tinyh264_allocator_scope_leave(
    const h2_pal_mem_api_t *previous);

typedef struct tinyh264_session {
    h2_pal_mem_api_t allocator;
    storage_t *decoder;
    uint8_t *input;
    size_t input_capacity;
    uint8_t *output;
    size_t output_capacity;
    h2_video_pixel_format_t output_format;
    uint32_t width;
    uint32_t height;
    int64_t pts_us;
    int64_t duration_us;
    int configured;
    int ready;
    int acquired;
    int eos;
} tinyh264_session_t;

struct h2_pal_video_decoder_frame {
    tinyh264_session_t *owner;
};

struct h2_pal_video_decoder_session {
    tinyh264_session_t state;
    h2_pal_video_decoder_frame_t frame;
};

static int find_baseline_sps(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i + 5u < size; ++i) {
        size_t prefix = 0u;
        if (bytes[i] == 0u && bytes[i + 1u] == 0u && bytes[i + 2u] == 1u) {
            prefix = 3u;
        } else if (i + 4u < size && bytes[i] == 0u && bytes[i + 1u] == 0u &&
                   bytes[i + 2u] == 0u && bytes[i + 3u] == 1u) {
            prefix = 4u;
        }
        if (prefix != 0u && i + prefix + 1u < size &&
            (bytes[i + prefix] & 0x1fu) == 7u) {
            return bytes[i + prefix + 1u] == 66u;
        }
    }
    return 0;
}

static int valid_annex_b(const void *data, size_t size) {
    const uint8_t *bytes = data;
    return size >= 4u && bytes[0] == 0u && bytes[1] == 0u &&
        (bytes[2] == 1u || (bytes[2] == 0u && bytes[3] == 1u));
}

static int clamp_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

static void yuv420_to_rgb565(
    uint8_t *output,
    const uint8_t *picture,
    uint32_t width,
    uint32_t height) {
    const size_t y_bytes = (size_t)width * height;
    const size_t chroma_width = width / 2u;
    const uint8_t *u_plane = picture + y_bytes;
    const uint8_t *v_plane = u_plane + y_bytes / 4u;
    for (uint32_t y = 0u; y < height; ++y) {
        uint16_t *destination =
            (uint16_t *)(void *)(output + (size_t)y * width * sizeof(uint16_t));
        const uint8_t *y_row = picture + (size_t)y * width;
        const uint8_t *u_row = u_plane + (size_t)(y / 2u) * chroma_width;
        const uint8_t *v_row = v_plane + (size_t)(y / 2u) * chroma_width;
        for (uint32_t x = 0u; x < width; ++x) {
            const int c = (int)y_row[x] - 16;
            const int d = (int)u_row[x / 2u] - 128;
            const int e = (int)v_row[x / 2u] - 128;
            const int red = clamp_byte((298 * c + 409 * e + 128) >> 8);
            const int green =
                clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
            const int blue = clamp_byte((298 * c + 516 * d + 128) >> 8);
            destination[x] =
                (uint16_t)(((uint16_t)(red & 0xf8) << 8) |
                           ((uint16_t)(green & 0xfc) << 3) |
                           ((uint16_t)blue >> 3));
        }
    }
}

static int has_parameter_sets(const storage_t *decoder) {
    int has_sps = 0;
    int has_pps = 0;
    for (size_t i = 0u; i < MAX_NUM_SEQ_PARAM_SETS; ++i) {
        has_sps |= decoder->sps[i] != NULL;
    }
    for (size_t i = 0u; i < MAX_NUM_PIC_PARAM_SETS; ++i) {
        has_pps |= decoder->pps[i] != NULL;
    }
    return has_sps && has_pps;
}

static void destroy_decoder(tinyh264_session_t *session) {
    if (session->decoder != NULL) {
        const h2_pal_mem_api_t *previous =
            h2_tinyh264_allocator_scope_enter(&session->allocator);
        h264bsdShutdown(session->decoder);
        h264bsdFree(session->decoder);
        h2_tinyh264_allocator_scope_leave(previous);
        session->decoder = NULL;
    }
}

static h2_pal_result_t reserve_input(
    tinyh264_session_t *session,
    size_t required) {
    if (required <= session->input_capacity) {
        return H2_PAL_OK;
    }
    uint8_t *replacement = h2_pal_mem_alloc(&session->allocator, required);
    if (replacement == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_pal_mem_free(&session->allocator, session->input);
    session->input = replacement;
    session->input_capacity = required;
    return H2_PAL_OK;
}

static h2_pal_result_t tiny_open(
    void *user,
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session) {
    (void)user;
    if (config->preferred_format != H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED &&
        config->preferred_format != H2_VIDEO_PIXEL_FORMAT_YUV420P &&
        config->preferred_format != H2_VIDEO_PIXEL_FORMAT_RGB565) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    h2_pal_video_decoder_session_t *session =
        h2_pal_mem_alloc(config->frame_allocator, sizeof(*session));
    if (session == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(*session));
    session->state.allocator = *config->frame_allocator;
    session->state.output_format =
        config->preferred_format == H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED
            ? H2_VIDEO_PIXEL_FORMAT_YUV420P
            : config->preferred_format;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t tiny_configure(
    void *user,
    h2_pal_video_decoder_session_t *opaque,
    const h2_video_decoder_stream_config_t *config) {
    (void)user;
    tinyh264_session_t *session = &opaque->state;
    if (session->configured || session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (config->codec != H2_VIDEO_CODEC_H264 ||
        config->bitstream_format != H2_VIDEO_BITSTREAM_H264_ANNEX_B) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (config->codec_config_size > UINT32_MAX ||
        !valid_annex_b(config->codec_config, config->codec_config_size)) {
        return H2_PAL_ERR_FORMAT;
    }
    if (!find_baseline_sps(config->codec_config, config->codec_config_size)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (config->visible_width != config->coded_width ||
        config->visible_height != config->coded_height ||
        (config->visible_width & 1u) != 0u ||
        (config->visible_height & 1u) != 0u) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (config->coded_width > UINT32_MAX / config->coded_height ||
        (size_t)config->coded_width * config->coded_height > SIZE_MAX / 3u) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const size_t pixels = (size_t)config->coded_width * config->coded_height;
    const size_t required =
        session->output_format == H2_VIDEO_PIXEL_FORMAT_RGB565
            ? pixels * sizeof(uint16_t)
            : pixels * 3u / 2u;
    uint8_t *output = h2_pal_mem_alloc(&session->allocator, required);
    const h2_pal_result_t input_result =
        reserve_input(session, config->codec_config_size);
    if (output == NULL || input_result != H2_PAL_OK) {
        h2_pal_mem_free(&session->allocator, output);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(session->input, config->codec_config, config->codec_config_size);
    const h2_pal_mem_api_t *previous =
        h2_tinyh264_allocator_scope_enter(&session->allocator);
    storage_t *decoder = h264bsdAlloc();
    const int init_ok = decoder != NULL && h264bsdInit(decoder, 0u) == 0u;
    uint8_t *ignored_picture = NULL;
    u32 ignored_width = 0u;
    u32 ignored_height = 0u;
    u32 decode_result = H264BSD_ERROR;
    if (init_ok) {
        decode_result = h264bsdDecode(
            decoder,
            session->input,
            (uint32_t)config->codec_config_size,
            &ignored_picture,
            &ignored_width,
            &ignored_height);
    }
    h2_tinyh264_allocator_scope_leave(previous);
    if (!init_ok || decode_result == H264BSD_MEMALLOC_ERROR ||
        !has_parameter_sets(decoder)) {
        if (decoder != NULL) {
            const h2_pal_mem_api_t *cleanup_previous =
                h2_tinyh264_allocator_scope_enter(&session->allocator);
            h264bsdShutdown(decoder);
            h264bsdFree(decoder);
            h2_tinyh264_allocator_scope_leave(cleanup_previous);
        }
        h2_pal_mem_free(&session->allocator, output);
        if (!init_ok || decode_result == H264BSD_MEMALLOC_ERROR) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        return H2_PAL_ERR_FORMAT;
    }
    session->decoder = decoder;
    session->output = output;
    session->output_capacity = required;
    session->width = config->visible_width;
    session->height = config->visible_height;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t tiny_submit(
    void *user,
    h2_pal_video_decoder_session_t *opaque,
    const h2_video_decoder_packet_t *packet) {
    (void)user;
    tinyh264_session_t *session = &opaque->state;
    if (!session->configured || session->eos) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (packet->flags != 0u) {
        session->eos = 1;
        return H2_PAL_OK;
    }
    if (session->ready || session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (packet->size > UINT32_MAX || !valid_annex_b(packet->data, packet->size)) {
        return H2_PAL_ERR_FORMAT;
    }
    const h2_pal_result_t input_result = reserve_input(session, packet->size);
    if (input_result != H2_PAL_OK) {
        return input_result;
    }
    memcpy(session->input, packet->data, packet->size);
    uint8_t *picture = NULL;
    u32 width = 0u;
    u32 height = 0u;
    const h2_pal_mem_api_t *previous =
        h2_tinyh264_allocator_scope_enter(&session->allocator);
    const u32 result = h264bsdDecode(
        session->decoder,
        session->input,
        (uint32_t)packet->size,
        &picture,
        &width,
        &height);
    h2_tinyh264_allocator_scope_leave(previous);
    if (result == H264BSD_ERROR || result == H264BSD_PARAM_SET_ERROR) {
        return H2_PAL_ERR_FORMAT;
    }
    if (result == H264BSD_MEMALLOC_ERROR) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (result == H264BSD_PIC_RDY) {
        const size_t bytes = (size_t)width * height * 3u / 2u;
        if (picture == NULL || width != session->width || height != session->height ||
            (session->output_format == H2_VIDEO_PIXEL_FORMAT_YUV420P &&
             bytes > session->output_capacity)) {
            return H2_PAL_ERR_FORMAT;
        }
        if (session->output_format == H2_VIDEO_PIXEL_FORMAT_RGB565) {
            yuv420_to_rgb565(session->output, picture, width, height);
        } else {
            memcpy(session->output, picture, bytes);
        }
        session->pts_us = packet->pts_us;
        session->duration_us = packet->duration_us;
        session->ready = 1;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t tiny_acquire(
    void *user,
    h2_pal_video_decoder_session_t *opaque,
    uint32_t timeout_ms,
    h2_pal_video_decoder_frame_t **out_frame) {
    (void)user;
    (void)timeout_ms;
    tinyh264_session_t *session = &opaque->state;
    if (session->acquired) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (!session->ready) {
        return session->eos ? H2_PAL_EXIT : H2_PAL_ERR_WOULD_BLOCK;
    }
    opaque->frame.owner = session;
    session->acquired = 1;
    *out_frame = &opaque->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t tiny_info(
    void *user,
    h2_pal_video_decoder_session_t *opaque,
    h2_pal_video_decoder_frame_t *frame,
    h2_video_frame_info_t *out_info) {
    (void)user;
    tinyh264_session_t *session = &opaque->state;
    if (!session->acquired || frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    out_info->format = session->output_format;
    out_info->width = session->width;
    out_info->height = session->height;
    const size_t y = (size_t)session->width * session->height;
    if (session->output_format == H2_VIDEO_PIXEL_FORMAT_RGB565) {
        out_info->plane_count = 1u;
        out_info->planes[0] = (h2_video_frame_plane_t){
            session->output,
            y * sizeof(uint16_t),
            session->width * sizeof(uint16_t),
        };
    } else {
        const size_t c = y / 4u;
        out_info->plane_count = 3u;
        out_info->planes[0] = (h2_video_frame_plane_t){
            session->output, y, session->width,
        };
        out_info->planes[1] = (h2_video_frame_plane_t){
            session->output + y, c, session->width / 2u,
        };
        out_info->planes[2] = (h2_video_frame_plane_t){
            session->output + y + c, c, session->width / 2u,
        };
    }
    out_info->pts_us = session->pts_us;
    out_info->duration_us = session->duration_us;
    return H2_PAL_OK;
}

static h2_pal_result_t tiny_release(
    void *user,
    h2_pal_video_decoder_session_t *opaque,
    h2_pal_video_decoder_frame_t *frame) {
    (void)user;
    tinyh264_session_t *session = &opaque->state;
    if (!session->acquired || frame->owner != session) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    session->acquired = 0;
    session->ready = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t tiny_reset(void *user, h2_pal_video_decoder_session_t *opaque) {
    (void)user;
    tinyh264_session_t *session = &opaque->state;
    if (session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    destroy_decoder(session);
    h2_pal_mem_free(&session->allocator, session->output);
    session->output = NULL;
    session->output_capacity = 0u;
    session->configured = 0;
    session->ready = 0;
    session->eos = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t tiny_close(void *user, h2_pal_video_decoder_session_t *opaque) {
    (void)user;
    tinyh264_session_t *session = &opaque->state;
    if (session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const h2_pal_mem_api_t allocator = session->allocator;
    destroy_decoder(session);
    h2_pal_mem_free(&allocator, session->input);
    h2_pal_mem_free(&allocator, session->output);
    h2_pal_mem_free(&allocator, opaque);
    return H2_PAL_OK;
}

static const h2_pal_video_decoder_vtable_t g_vtable = {
    .open = tiny_open,
    .configure = tiny_configure,
    .submit_packet = tiny_submit,
    .acquire_frame = tiny_acquire,
    .frame_get_info = tiny_info,
    .release_frame = tiny_release,
    .reset = tiny_reset,
    .close = tiny_close,
};

static const h2_pal_video_decoder_api_t g_api = {
    .user = NULL,
    .vtable = &g_vtable,
};

const h2_pal_video_decoder_api_t *h2_tinyh264_video_decoder_api(void) {
    return &g_api;
}
