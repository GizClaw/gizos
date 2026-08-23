#ifndef H2_PAL_VIDEO_DECODER_H
#define H2_PAL_VIDEO_DECODER_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_VIDEO_DECODER_MAX_PLANES 4u

/** @brief Codec accepted by the packet-fed Video Decoder PAL. */
typedef enum h2_video_codec {
    H2_VIDEO_CODEC_H264 = 1,
} h2_video_codec_t;

/** @brief Compressed bitstream framing accepted by the Video Decoder PAL. */
typedef enum h2_video_bitstream_format {
    H2_VIDEO_BITSTREAM_H264_ANNEX_B = 1,
} h2_video_bitstream_format_t;

/** @brief CPU-readable decoded-frame pixel format. */
typedef enum h2_video_pixel_format {
    H2_VIDEO_PIXEL_FORMAT_UNSPECIFIED = 0,
    H2_VIDEO_PIXEL_FORMAT_NV12,
    H2_VIDEO_PIXEL_FORMAT_YUV420P,
    H2_VIDEO_PIXEL_FORMAT_RGB565,
    H2_VIDEO_PIXEL_FORMAT_RGB888,
    H2_VIDEO_PIXEL_FORMAT_RGBA8888,
} h2_video_pixel_format_t;

/** @brief Immutable options copied by open into a new decoder session. */
typedef struct h2_video_decoder_config {
    /** Required allocator for session state and every PAL-visible output plane. */
    const h2_pal_mem_api_t *frame_allocator;
    /** Requested CPU-readable output, or UNSPECIFIED to select a backend default. */
    h2_video_pixel_format_t preferred_format;
} h2_video_decoder_config_t;

/** @brief Borrowed stream metadata consumed synchronously by configure. */
typedef struct h2_video_decoder_stream_config {
    h2_video_codec_t codec;
    h2_video_bitstream_format_t bitstream_format;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t visible_width;
    uint32_t visible_height;
    /** Borrowed codec configuration, such as Annex-B SPS/PPS with start codes. */
    const void *codec_config;
    size_t codec_config_size;
} h2_video_decoder_stream_config_t;

enum {
    /** Packet carries end-of-stream and therefore has no payload. */
    H2_VIDEO_DECODER_PACKET_END_OF_STREAM = 1u << 0,
};

/** @brief One complete compressed access unit borrowed during submit_packet. */
typedef struct h2_video_decoder_packet {
    const void *data;
    size_t size;
    int64_t pts_us;
    int64_t dts_us;
    int64_t duration_us;
    uint32_t flags;
} h2_video_decoder_packet_t;

/** @brief One CPU-readable plane borrowed from an acquired frame. */
typedef struct h2_video_frame_plane {
    /** Read-only bytes valid until the matching frame is released. */
    const void *data;
    /** Total readable bytes in this plane, including any row padding. */
    size_t bytes;
    /** Byte distance between adjacent rows. */
    size_t stride_bytes;
} h2_video_frame_plane_t;

/** @brief Metadata and borrowed planes for one acquired decoded frame. */
typedef struct h2_video_frame_info {
    h2_video_pixel_format_t format;
    uint32_t width;
    uint32_t height;
    h2_video_frame_plane_t planes[H2_VIDEO_DECODER_MAX_PLANES];
    uint8_t plane_count;
    int64_t pts_us;
    int64_t duration_us;
} h2_video_frame_info_t;

/** @brief Opaque decoder session owned by the caller from open through close. */
typedef struct h2_pal_video_decoder_session h2_pal_video_decoder_session_t;
/** @brief Opaque acquired frame owned by its session until release_frame. */
typedef struct h2_pal_video_decoder_frame h2_pal_video_decoder_frame_t;
typedef struct h2_pal_video_decoder_api h2_pal_video_decoder_api_t;

/**
 * @brief Packet-fed video decoder backend operations.
 *
 * A session is single-caller and permits at most one acquired frame. Packet
 * bytes and stream configuration are borrowed only for their synchronous call.
 * H2_PAL_OK from submit_packet consumes the complete packet; WOULD_BLOCK
 * consumes nothing. Public frame planes are allocator-backed, read-only, and
 * valid until release_frame. Reset and close require no acquired frame.
 */
typedef struct h2_pal_video_decoder_vtable {
    h2_pal_result_t (*open)(
        void *user,
        const h2_video_decoder_config_t *config,
        h2_pal_video_decoder_session_t **out_session);
    h2_pal_result_t (*configure)(
        void *user,
        h2_pal_video_decoder_session_t *session,
        const h2_video_decoder_stream_config_t *config);
    h2_pal_result_t (*submit_packet)(
        void *user,
        h2_pal_video_decoder_session_t *session,
        const h2_video_decoder_packet_t *packet);
    h2_pal_result_t (*acquire_frame)(
        void *user,
        h2_pal_video_decoder_session_t *session,
        uint32_t timeout_ms,
        h2_pal_video_decoder_frame_t **out_frame);
    h2_pal_result_t (*frame_get_info)(
        void *user,
        h2_pal_video_decoder_session_t *session,
        h2_pal_video_decoder_frame_t *frame,
        h2_video_frame_info_t *out_info);
    h2_pal_result_t (*release_frame)(
        void *user,
        h2_pal_video_decoder_session_t *session,
        h2_pal_video_decoder_frame_t *frame);
    h2_pal_result_t (*reset)(void *user, h2_pal_video_decoder_session_t *session);
    h2_pal_result_t (*close)(void *user, h2_pal_video_decoder_session_t *session);
} h2_pal_video_decoder_vtable_t;

/** @brief Borrowed video decoder capability object using the PAL user/vtable shape. */
struct h2_pal_video_decoder_api {
    void *user;
    const h2_pal_video_decoder_vtable_t *vtable;
};

static inline int h2_video_decoder_allocator_is_valid(
    const h2_pal_mem_api_t *allocator) {
    return allocator != NULL && allocator->vtable != NULL &&
        allocator->vtable->alloc != NULL && allocator->vtable->free != NULL;
}

/** @brief Open an unconfigured packet-fed decoder session. */
static inline h2_pal_result_t h2_pal_video_decoder_open(
    const h2_pal_video_decoder_api_t *decoder,
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session) {
    if (out_session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_session = NULL;
    if (decoder == NULL || decoder->vtable == NULL || decoder->vtable->open == NULL ||
        config == NULL || !h2_video_decoder_allocator_is_valid(config->frame_allocator)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->open(decoder->user, config, out_session);
}

/** @brief Configure a newly opened or reset session from borrowed metadata. */
static inline h2_pal_result_t h2_pal_video_decoder_configure(
    const h2_pal_video_decoder_api_t *decoder,
    h2_pal_video_decoder_session_t *session,
    const h2_video_decoder_stream_config_t *config) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->configure == NULL || session == NULL || config == NULL ||
        config->coded_width == 0u || config->coded_height == 0u ||
        config->visible_width == 0u || config->visible_height == 0u ||
        config->visible_width > config->coded_width ||
        config->visible_height > config->coded_height ||
        config->codec_config == NULL || config->codec_config_size == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->configure(decoder->user, session, config);
}

/** @brief Submit one complete compressed access unit or explicit EOS packet. */
static inline h2_pal_result_t h2_pal_video_decoder_submit_packet(
    const h2_pal_video_decoder_api_t *decoder,
    h2_pal_video_decoder_session_t *session,
    const h2_video_decoder_packet_t *packet) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->submit_packet == NULL || session == NULL || packet == NULL ||
        (packet->flags & ~H2_VIDEO_DECODER_PACKET_END_OF_STREAM) != 0u ||
        packet->duration_us < 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const int is_eos =
        (packet->flags & H2_VIDEO_DECODER_PACKET_END_OF_STREAM) != 0u;
    if ((is_eos && (packet->data != NULL || packet->size != 0u)) ||
        (!is_eos && (packet->data == NULL || packet->size == 0u))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->submit_packet(decoder->user, session, packet);
}

/** @brief Acquire the next decoded frame. */
static inline h2_pal_result_t h2_pal_video_decoder_acquire_frame(
    const h2_pal_video_decoder_api_t *decoder,
    h2_pal_video_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_video_decoder_frame_t **out_frame) {
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

/** @brief Copy metadata and borrowed CPU-readable planes from an acquired frame. */
static inline h2_pal_result_t h2_pal_video_decoder_frame_get_info(
    const h2_pal_video_decoder_api_t *decoder,
    h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame,
    h2_video_frame_info_t *out_info) {
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

/** @brief Return exactly one acquired frame to its owning session. */
static inline h2_pal_result_t h2_pal_video_decoder_release_frame(
    const h2_pal_video_decoder_api_t *decoder,
    h2_pal_video_decoder_session_t *session,
    h2_pal_video_decoder_frame_t *frame) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->release_frame == NULL || session == NULL || frame == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->release_frame(decoder->user, session, frame);
}

/** @brief Discard stream state and return a session to the unconfigured state. */
static inline h2_pal_result_t h2_pal_video_decoder_reset(
    const h2_pal_video_decoder_api_t *decoder,
    h2_pal_video_decoder_session_t *session) {
    if (decoder == NULL || decoder->vtable == NULL ||
        decoder->vtable->reset == NULL || session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return decoder->vtable->reset(decoder->user, session);
}

/** @brief Release a session that has no acquired frame. */
static inline h2_pal_result_t h2_pal_video_decoder_close(
    const h2_pal_video_decoder_api_t *decoder,
    h2_pal_video_decoder_session_t *session) {
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
