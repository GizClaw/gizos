#include "h2_smoke_mp4_player.h"
#include "h2_smoke_mp4_player_task_names.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_MP4_PLAYER_MAX_AUDIO_CHANNELS 8u
#ifndef H2_MP4_PLAYER_VIDEO_BUFFER_COUNT
#define H2_MP4_PLAYER_VIDEO_BUFFER_COUNT 3u
#endif
#ifndef H2_MP4_PLAYER_DECODER_TASK_STACK_BYTES
#define H2_MP4_PLAYER_DECODER_TASK_STACK_BYTES 32768u
#endif
#ifndef H2_MP4_PLAYER_PROGRESS_LOG_INTERVAL
#define H2_MP4_PLAYER_PROGRESS_LOG_INTERVAL 0u
#endif
#define H2_MP4_PLAYER_QUEUE_POLL_MS 50u

#if H2_MP4_PLAYER_VIDEO_BUFFER_COUNT < 1
#error "H2_MP4_PLAYER_VIDEO_BUFFER_COUNT must be at least 1"
#endif

typedef struct player_presentation_buffer {
    uint8_t *video;
    size_t video_capacity;
    size_t video_stride_bytes;
    int16_t *pcm;
    size_t pcm_capacity_values;
    size_t pcm_values;
    int64_t pts_us;
    int64_t duration_us;
    uint32_t generation;
    int consumers;
} player_presentation_buffer_t;

typedef struct player_pipeline {
    h2_runtime_t *runtime;
    const h2_smoke_mp4_player_config_t *config;
    h2_mp4_decoder_t *decoder;
    h2_pal_audio_track_t *track;
    h2_display_info_t display;
    h2_display_rect_t video_rect;
    uint16_t audio_block_samples;
    uint32_t audio_sample_rate_hz;
    uint8_t audio_channels;
    int16_t *pending_pcm;
    size_t pending_values;
    h2_pal_queue_t *free_video;
    h2_pal_queue_t *ready_video;
    h2_pal_queue_t *ready_audio;
    h2_pal_mutex_t *buffer_mutex;
    uint32_t buffer_release_count;
    player_presentation_buffer_t buffers[H2_MP4_PLAYER_VIDEO_BUFFER_COUNT];
    atomic_int result;
    atomic_int stop;
    atomic_size_t frame_count;
} player_pipeline_t;

typedef struct player_fs_source {
    const h2_pal_fs_api_t *fs;
    h2_pal_fs_file_t *file;
    uint64_t position;
} player_fs_source_t;

static h2_pal_result_t player_fs_read_at(
    void *user,
    uint64_t offset,
    void *buffer,
    size_t capacity,
    size_t *out_read) {
    player_fs_source_t *source = user;
    if (offset != source->position) {
        const h2_pal_result_t seek_result =
            (h2_pal_result_t)h2_pal_fs_seek(
                source->fs,
                source->file,
                offset);
        if (seek_result != H2_PAL_OK) {
            return seek_result;
        }
    }
    const h2_pal_result_t result =
        (h2_pal_result_t)h2_pal_fs_read(
        source->fs, source->file, buffer, capacity, out_read);
    if (result == H2_PAL_OK) {
        if (*out_read > UINT64_MAX - offset) {
            return H2_PAL_ERR_FORMAT;
        }
        source->position = offset + *out_read;
    }
    return result;
}

static void player_log(
    h2_runtime_t *runtime,
    h2_pal_log_level_t level,
    const char *message) {
    if (runtime->log != NULL) {
        (void)h2_pal_log_write(runtime->log, level, "mp4-player", message);
    }
}

static h2_pal_result_t player_fail(
    h2_runtime_t *runtime,
    const char *stage,
    h2_pal_result_t result) {
    char message[H2_PAL_LOG_MESSAGE_MAX];
    (void)snprintf(message, sizeof(message), "H2_MP4_PLAYER_FAIL stage=%s rc=%d", stage, result);
    player_log(runtime, H2_PAL_LOG_ERROR, message);
    return result;
}

static h2_pal_result_t clear_display_black(
    h2_runtime_t *runtime,
    const h2_display_info_t *display) {
    const size_t stride_bytes = (size_t)display->width * sizeof(uint16_t);
    if ((size_t)display->height > SIZE_MAX / stride_bytes) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const size_t buffer_bytes = stride_bytes * (size_t)display->height;
    void *buffer = h2_pal_mem_alloc(runtime->mem, buffer_bytes);
    if (buffer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(buffer, 0, buffer_bytes);
    const h2_display_rect_t rect = {
        .x = 0,
        .y = 0,
        .width = display->width,
        .height = display->height,
    };
    h2_pal_result_t result =
        (h2_pal_result_t)h2_pal_display_draw_bitmap(
            runtime->display,
            &rect,
            buffer,
            stride_bytes,
            H2_DISPLAY_PIXEL_RGB565);
    if (result == H2_PAL_OK) {
        result = (h2_pal_result_t)h2_pal_display_present(runtime->display);
    }
    h2_pal_mem_free(runtime->mem, buffer);
    return result;
}

static h2_pal_result_t pace_frame(
    h2_runtime_t *runtime,
    const h2_smoke_mp4_player_config_t *config,
    uint64_t base_clock_ms,
    int64_t base_pts_us,
    int64_t pts_us) {
    uint64_t now_ms = 0u;
    h2_pal_result_t result = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    const uint64_t target = pts_us > base_pts_us
        ? base_clock_ms + (uint64_t)(pts_us - base_pts_us) / 1000u
        : base_clock_ms;
    while (result == H2_PAL_OK && now_ms < target) {
        if (config->should_stop != NULL && config->should_stop(config->stop_user)) {
            return H2_PAL_EXIT;
        }
        uint64_t delay = target - now_ms;
        if (delay > 50u) {
            delay = 50u;
        }
        result = h2_pal_time_sleep_ms(runtime->time, (uint32_t)delay);
        if (result == H2_PAL_OK) {
            result = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
        }
    }
    return result;
}

static int frame_plane_covers_rows(
    const h2_mp4_decoder_frame_info_t *info) {
    if (info->width == 0u || info->height == 0u) {
        return 0;
    }
    const size_t row_bytes = (size_t)info->width * sizeof(uint16_t);
    if (info->video_planes[0].stride_bytes < row_bytes) {
        return 0;
    }
    const size_t preceding_rows = (size_t)info->height - 1u;
    if (preceding_rows != 0u &&
        info->video_planes[0].stride_bytes >
            (SIZE_MAX - row_bytes) / preceding_rows) {
        return 0;
    }
    return info->video_planes[0].bytes >=
        preceding_rows * info->video_planes[0].stride_bytes + row_bytes;
}

static h2_pal_result_t write_audio_block(
    h2_pal_audio_track_t *track,
    int16_t *samples,
    uint16_t samples_per_channel,
    uint32_t sample_rate_hz,
    uint8_t channels,
    uint32_t timeout_ms) {
    const size_t values = (size_t)samples_per_channel * channels;
    const h2_audio_frame_t audio = {
        .data = samples,
        .capacity = values * sizeof(int16_t),
        .bytes = values * sizeof(int16_t),
        .sample_rate_hz = sample_rate_hz,
        .samples_per_channel = samples_per_channel,
        .channels = channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return (h2_pal_result_t)h2_pal_audio_track_write(
        track, &audio, timeout_ms);
}

static int pipeline_should_stop(player_pipeline_t *pipeline) {
    return atomic_load(&pipeline->stop) != 0;
}

static void pipeline_stop(player_pipeline_t *pipeline) {
    atomic_store(&pipeline->stop, 1);
}

static int pipeline_poll_stop(player_pipeline_t *pipeline) {
    if (!pipeline_should_stop(pipeline) &&
        pipeline->config->should_stop != NULL &&
        pipeline->config->should_stop(pipeline->config->stop_user)) {
        pipeline_stop(pipeline);
    }
    return pipeline_should_stop(pipeline);
}

static void pipeline_fail(
    player_pipeline_t *pipeline,
    const char *stage,
    h2_pal_result_t result) {
    int expected = H2_PAL_OK;
    if (atomic_compare_exchange_strong(
            &pipeline->result,
            &expected,
            result)) {
        (void)player_fail(pipeline->runtime, stage, result);
    }
    pipeline_stop(pipeline);
}

static h2_pal_result_t pipeline_queue_send(
    player_pipeline_t *pipeline,
    h2_pal_queue_t *queue,
    const size_t *item) {
    for (;;) {
        if (pipeline_should_stop(pipeline)) {
            return H2_PAL_EXIT;
        }
        const h2_pal_result_t result =
            (h2_pal_result_t)h2_pal_queue_send(
                pipeline->runtime->queue,
                queue,
                item,
                H2_MP4_PLAYER_QUEUE_POLL_MS);
        if (result == H2_PAL_OK || result == H2_PAL_ERR_CLOSED) {
            return result;
        }
        if (result != H2_PAL_ERR_TIMEOUT) {
            return result;
        }
    }
}

static h2_pal_result_t pipeline_queue_recv(
    player_pipeline_t *pipeline,
    h2_pal_queue_t *queue,
    size_t *out_item,
    int stop_is_exit,
    int poll_config_stop) {
    for (;;) {
        if (stop_is_exit &&
            (poll_config_stop
                 ? pipeline_poll_stop(pipeline)
                 : pipeline_should_stop(pipeline))) {
            return H2_PAL_EXIT;
        }
        const h2_pal_result_t result =
            (h2_pal_result_t)h2_pal_queue_recv(
                pipeline->runtime->queue,
                queue,
                out_item,
                H2_MP4_PLAYER_QUEUE_POLL_MS);
        if (result == H2_PAL_OK || result == H2_PAL_ERR_CLOSED) {
            return result;
        }
        if (result != H2_PAL_ERR_TIMEOUT) {
            return result;
        }
    }
}

static void release_presentation_buffer(
    player_pipeline_t *pipeline,
    size_t index) {
    player_presentation_buffer_t *buffer = &pipeline->buffers[index];
    if (h2_pal_mutex_lock(
            pipeline->runtime->sync, pipeline->buffer_mutex) != H2_PAL_OK) {
        return;
    }
    const int remaining = --buffer->consumers;
    const int recycle = remaining == 0;
    const uint32_t release_count = ++pipeline->buffer_release_count;
    (void)h2_pal_mutex_unlock(
        pipeline->runtime->sync, pipeline->buffer_mutex);
    if (release_count <= 10u) {
        char message[H2_PAL_LOG_MESSAGE_MAX];
        (void)snprintf(
            message, sizeof(message),
            "H2_MP4_PLAYER_BUFFER_RELEASE count=%u index=%u remaining=%d",
            (unsigned)release_count, (unsigned)index, remaining);
        player_log(pipeline->runtime, H2_PAL_LOG_INFO, message);
    }
    if (!recycle) return;
    while (h2_pal_queue_send(
               pipeline->runtime->queue,
               pipeline->free_video,
               &index,
               H2_MP4_PLAYER_QUEUE_POLL_MS) == H2_PAL_ERR_TIMEOUT) {
    }
}

static h2_pal_result_t reserve_pcm(
    player_pipeline_t *pipeline,
    player_presentation_buffer_t *buffer,
    size_t required_values) {
    if (required_values <= buffer->pcm_capacity_values) {
        return H2_PAL_OK;
    }
    if (required_values > SIZE_MAX / sizeof(int16_t)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    int16_t *replacement = h2_pal_mem_alloc(
        pipeline->runtime->mem,
        required_values * sizeof(int16_t));
    if (replacement == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_pal_mem_free(pipeline->runtime->mem, buffer->pcm);
    buffer->pcm = replacement;
    buffer->pcm_capacity_values = required_values;
    return H2_PAL_OK;
}

static h2_pal_result_t copy_presentation_frame(
    player_pipeline_t *pipeline,
    player_presentation_buffer_t *buffer,
    const h2_mp4_decoder_frame_info_t *info,
    uint32_t generation) {
    if (info->video_format != H2_VIDEO_PIXEL_FORMAT_RGB565 ||
        info->width != (uint32_t)pipeline->video_rect.width ||
        info->height != (uint32_t)pipeline->video_rect.height ||
        info->video_plane_count != 1u ||
        info->video_planes[0].data == NULL ||
        !frame_plane_covers_rows(info) ||
        info->pcm_samples_per_channel > UINT16_MAX ||
        (pipeline->track != NULL &&
         (info->pcm_sample_rate_hz != pipeline->audio_sample_rate_hz ||
          info->pcm_channels != pipeline->audio_channels)) ||
        (info->pcm_samples_per_channel != 0u &&
         (info->pcm == NULL ||
          info->pcm_channels == 0u ||
          info->pcm_channels > H2_MP4_PLAYER_MAX_AUDIO_CHANNELS))) {
        return H2_PAL_ERR_FORMAT;
    }
    const size_t row_bytes = (size_t)info->width * sizeof(uint16_t);
    if (row_bytes > buffer->video_capacity / info->height) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    const uint8_t *source = info->video_planes[0].data;
    for (uint32_t row = 0u; row < info->height; ++row) {
        memcpy(
            buffer->video + (size_t)row * row_bytes,
            source + (size_t)row * info->video_planes[0].stride_bytes,
            row_bytes);
    }
    const size_t pcm_values =
        info->pcm_samples_per_channel * info->pcm_channels;
    h2_pal_result_t result = reserve_pcm(pipeline, buffer, pcm_values);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (pcm_values != 0u) {
        memcpy(buffer->pcm, info->pcm, pcm_values * sizeof(int16_t));
    }
    buffer->video_stride_bytes = row_bytes;
    buffer->pcm_values = pcm_values;
    buffer->pts_us = info->pts_us;
    buffer->duration_us = info->duration_us;
    buffer->generation = generation;
    return H2_PAL_OK;
}

static void decoder_task(void *context) {
    player_pipeline_t *pipeline = context;
    uint32_t generation = 0u;
    size_t decoded_frames = 0u;
    player_log(
        pipeline->runtime, H2_PAL_LOG_INFO,
        "H2_MP4_PLAYER_DECODER stage=enter");
    for (;;) {
        size_t index = 0u;
        h2_pal_result_t result = pipeline_queue_recv(
            pipeline,
            pipeline->free_video,
            &index,
            1,
            0);
        if (result == H2_PAL_EXIT || result == H2_PAL_ERR_CLOSED) {
            break;
        }
        if (result != H2_PAL_OK ||
            index >= H2_MP4_PLAYER_VIDEO_BUFFER_COUNT) {
            pipeline_fail(
                pipeline,
                "free-buffer",
                result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result);
            break;
        }
        const int trace_frame = decoded_frames < 5u;
        if (trace_frame) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_DECODER stage=free-buffer");
        }

        h2_mp4_decoder_frame_t *frame = NULL;
        if (trace_frame) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_DECODER stage=acquire-before");
        }
        result = h2_mp4_decoder_acquire_frame(
            pipeline->decoder,
            pipeline->config->acquire_timeout_ms,
            &frame);
        if (trace_frame) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_DECODER stage=acquire-after");
        }
        if (result == H2_PAL_EXIT && pipeline->config->looping) {
            result = h2_mp4_decoder_seek(pipeline->decoder, 0);
            if (result == H2_PAL_OK) {
                ++generation;
                player_log(
                    pipeline->runtime,
                    H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_LOOP");
                (void)h2_pal_queue_send(
                    pipeline->runtime->queue,
                    pipeline->free_video,
                    &index,
                    H2_PAL_QUEUE_WAIT_FOREVER);
                continue;
            }
        }
        if (result == H2_PAL_EXIT) {
            (void)h2_pal_queue_send(
                pipeline->runtime->queue,
                pipeline->free_video,
                &index,
                H2_PAL_QUEUE_WAIT_FOREVER);
            break;
        }
        if (result != H2_PAL_OK) {
            pipeline_fail(pipeline, "acquire", result);
            break;
        }

        h2_mp4_decoder_frame_info_t info = {0};
        if (trace_frame) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_DECODER stage=frame-info-before");
        }
        result = h2_mp4_decoder_frame_get_info(
            pipeline->decoder,
            frame,
            &info);
        if (trace_frame) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_DECODER stage=frame-info-after");
        }
        if (result == H2_PAL_OK) {
            result = copy_presentation_frame(
                pipeline,
                &pipeline->buffers[index],
                &info,
                generation);
        }
        if (trace_frame) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_DECODER stage=copy-after");
        }
        const h2_pal_result_t release =
            h2_mp4_decoder_release_frame(pipeline->decoder, frame);
        if (result == H2_PAL_OK) {
            result = release;
        }
        if (result != H2_PAL_OK) {
            pipeline_fail(pipeline, "decode-frame", result);
            break;
        }

        const int consumers = pipeline->track == NULL ? 1 : 2;
        pipeline->buffers[index].consumers = consumers;
        if (pipeline->track != NULL) {
            if (trace_frame) {
                player_log(
                    pipeline->runtime, H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_DECODER stage=ready-audio-before");
            }
            result = pipeline_queue_send(
                pipeline,
                pipeline->ready_audio,
                &index);
            if (trace_frame) {
                player_log(
                    pipeline->runtime, H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_DECODER stage=ready-audio-after");
            }
            /* Audio may need several decoded frames to fill its first block.
             * Keep producing frames; waiting here would starve that writer. */
        }
        if (result == H2_PAL_OK) {
            result = pipeline_queue_send(
                pipeline,
                pipeline->ready_video,
                &index);
        }
        if (trace_frame) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_DECODER stage=ready-video-after");
            char message[H2_PAL_LOG_MESSAGE_MAX];
            (void)snprintf(
                message, sizeof(message),
                "H2_MP4_PLAYER_DECODER queued=%u buffer=%u pcm_values=%u",
                (unsigned)(decoded_frames + 1u), (unsigned)index,
                (unsigned)pipeline->buffers[index].pcm_values);
            player_log(pipeline->runtime, H2_PAL_LOG_INFO, message);
        }
        if (result != H2_PAL_OK) {
            if (result != H2_PAL_EXIT && result != H2_PAL_ERR_CLOSED) {
                pipeline_fail(pipeline, "ready-buffer", result);
            }
            break;
        }
        ++decoded_frames;
    }
    (void)h2_pal_queue_close(
        pipeline->runtime->queue,
        pipeline->ready_video);
    if (pipeline->ready_audio != NULL) {
        (void)h2_pal_queue_close(
            pipeline->runtime->queue,
            pipeline->ready_audio);
    }
}

static void video_writer_task(void *context) {
    player_pipeline_t *pipeline = context;
    uint64_t base_clock_ms = 0u;
    int64_t base_pts_us = 0;
    uint32_t generation = 0u;
    int have_clock = 0;
    size_t video_buffers = 0u;
    for (;;) {
        size_t index = 0u;
        h2_pal_result_t result = pipeline_queue_recv(
            pipeline,
            pipeline->ready_video,
            &index,
            1,
            1);
        if (result == H2_PAL_EXIT || result == H2_PAL_ERR_CLOSED) {
            break;
        }
        if (result != H2_PAL_OK ||
            index >= H2_MP4_PLAYER_VIDEO_BUFFER_COUNT) {
            pipeline_fail(
                pipeline,
                "video-ready",
                result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result);
            break;
        }
        player_presentation_buffer_t *buffer =
            &pipeline->buffers[index];
        const int trace_buffer = video_buffers < 5u;
        if (trace_buffer) {
            char message[H2_PAL_LOG_MESSAGE_MAX];
            (void)snprintf(
                message, sizeof(message),
                "H2_MP4_PLAYER_VIDEO recv=%u buffer=%u pts_ms=%u",
                (unsigned)(video_buffers + 1u), (unsigned)index,
                (unsigned)(buffer->pts_us / 1000));
            player_log(pipeline->runtime, H2_PAL_LOG_INFO, message);
        }
        if (pipeline_should_stop(pipeline)) {
            release_presentation_buffer(pipeline, index);
            continue;
        }
        if (!have_clock || generation != buffer->generation) {
            result = h2_pal_time_get_monotonic_ms(
                pipeline->runtime->time,
                &base_clock_ms);
            base_pts_us = buffer->pts_us;
            generation = buffer->generation;
            have_clock = result == H2_PAL_OK;
        }
        if (result == H2_PAL_OK) {
            if (trace_buffer) {
                player_log(pipeline->runtime, H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_VIDEO stage=pace-before");
            }
            result = pace_frame(
                pipeline->runtime,
                pipeline->config,
                base_clock_ms,
                base_pts_us,
                buffer->pts_us);
            if (trace_buffer) {
                player_log(pipeline->runtime, H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_VIDEO stage=pace-after");
            }
        }
        if (result == H2_PAL_OK) {
            if (trace_buffer) {
                player_log(pipeline->runtime, H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_VIDEO stage=draw-before");
            }
            result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
                pipeline->runtime->display,
                &pipeline->video_rect,
                buffer->video,
                buffer->video_stride_bytes,
                H2_DISPLAY_PIXEL_RGB565);
            if (trace_buffer) {
                player_log(pipeline->runtime, H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_VIDEO stage=draw-after");
            }
        }
        if (result == H2_PAL_OK) {
            if (trace_buffer) {
                player_log(pipeline->runtime, H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_VIDEO stage=present-before");
            }
            result = (h2_pal_result_t)h2_pal_display_present(
                pipeline->runtime->display);
            if (trace_buffer) {
                player_log(pipeline->runtime, H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_VIDEO stage=present-after");
            }
        }
        if (result == H2_PAL_EXIT) {
            pipeline_stop(pipeline);
        } else if (result != H2_PAL_OK) {
            pipeline_fail(pipeline, "video-write", result);
        } else {
            const size_t frame_count =
                atomic_fetch_add(&pipeline->frame_count, 1u) + 1u;
            if (frame_count == 1u) {
                player_log(
                    pipeline->runtime,
                    H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_FRAME index=1");
                player_log(
                    pipeline->runtime,
                    H2_PAL_LOG_INFO,
                    "H2_MP4_PLAYER_READY");
                if (pipeline->config->on_ready != NULL) {
                    result = pipeline->config->on_ready(
                        pipeline->config->ready_user);
                    if (result != H2_PAL_OK) {
                        pipeline_fail(pipeline, "ready", result);
                    }
                }
            }
#if H2_MP4_PLAYER_PROGRESS_LOG_INTERVAL > 0
            if (frame_count % H2_MP4_PLAYER_PROGRESS_LOG_INTERVAL == 0u) {
                char message[H2_PAL_LOG_MESSAGE_MAX];
                (void)snprintf(
                    message,
                    sizeof(message),
                    "H2_MP4_PLAYER_FRAME index=%zu",
                    frame_count);
                player_log(
                    pipeline->runtime,
                    H2_PAL_LOG_INFO,
                    message);
            }
#endif
            if (pipeline->config->max_frames != 0u &&
                frame_count >= pipeline->config->max_frames) {
                pipeline_stop(pipeline);
            }
        }
        if (trace_buffer) {
            player_log(pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_VIDEO stage=release-before");
        }
        release_presentation_buffer(pipeline, index);
        if (trace_buffer) {
            player_log(pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_VIDEO stage=release-after");
        }
        ++video_buffers;
    }
}

static void audio_writer_task(void *context) {
    player_pipeline_t *pipeline = context;
    const size_t block_values =
        (size_t)pipeline->audio_block_samples *
        pipeline->audio_channels;
    int ready_logged = 0;
    size_t audio_buffers = 0u;
    player_log(
        pipeline->runtime, H2_PAL_LOG_INFO,
        "H2_MP4_PLAYER_AUDIO stage=enter");
    for (;;) {
        size_t index = 0u;
        h2_pal_result_t result = pipeline_queue_recv(
            pipeline,
            pipeline->ready_audio,
            &index,
            0,
            0);
        if (result == H2_PAL_ERR_CLOSED) {
            break;
        }
        if (result != H2_PAL_OK ||
            index >= H2_MP4_PLAYER_VIDEO_BUFFER_COUNT) {
            pipeline_fail(
                pipeline,
                "audio-ready",
                result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result);
            break;
        }
        player_presentation_buffer_t *buffer =
            &pipeline->buffers[index];
        const int trace_buffer = audio_buffers < 5u;
        if (trace_buffer) {
            char message[H2_PAL_LOG_MESSAGE_MAX];
            (void)snprintf(
                message, sizeof(message),
                "H2_MP4_PLAYER_AUDIO recv=%u buffer=%u pcm_values=%u",
                (unsigned)(audio_buffers + 1u), (unsigned)index,
                (unsigned)buffer->pcm_values);
            player_log(pipeline->runtime, H2_PAL_LOG_INFO, message);
        }
        if (!pipeline_should_stop(pipeline)) {
            size_t consumed = 0u;
            while (result == H2_PAL_OK && consumed < buffer->pcm_values) {
                size_t copy_values =
                    block_values - pipeline->pending_values;
                if (copy_values > buffer->pcm_values - consumed) {
                    copy_values = buffer->pcm_values - consumed;
                }
                memcpy(
                    pipeline->pending_pcm + pipeline->pending_values,
                    buffer->pcm + consumed,
                    copy_values * sizeof(int16_t));
                pipeline->pending_values += copy_values;
                consumed += copy_values;
                if (pipeline->pending_values == block_values) {
                    if (trace_buffer) {
                        player_log(
                            pipeline->runtime, H2_PAL_LOG_INFO,
                            "H2_MP4_PLAYER_AUDIO stage=write-before");
                    }
                    result = write_audio_block(
                        pipeline->track,
                        pipeline->pending_pcm,
                        pipeline->audio_block_samples,
                        pipeline->audio_sample_rate_hz,
                        pipeline->audio_channels,
                        pipeline->config->acquire_timeout_ms);
                    if (trace_buffer) {
                        player_log(
                            pipeline->runtime, H2_PAL_LOG_INFO,
                            "H2_MP4_PLAYER_AUDIO stage=write-after");
                    }
                    pipeline->pending_values = 0u;
                    if (result == H2_PAL_OK && !ready_logged) {
                        player_log(
                            pipeline->runtime,
                            H2_PAL_LOG_INFO,
                            "H2_MP4_PLAYER_AUDIO_READY");
                        ready_logged = 1;
                    }
                }
            }
            if (result != H2_PAL_OK) {
                pipeline_fail(pipeline, "audio-write", result);
            }
        }
        if (trace_buffer) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_AUDIO stage=release-before");
        }
        release_presentation_buffer(pipeline, index);
        if (trace_buffer) {
            player_log(
                pipeline->runtime, H2_PAL_LOG_INFO,
                "H2_MP4_PLAYER_AUDIO stage=release-after");
        }
        ++audio_buffers;
    }
    if (atomic_load(&pipeline->result) == H2_PAL_OK &&
        pipeline->pending_values != 0u) {
        const uint8_t channels = pipeline->audio_channels;
        const size_t flush_values =
            (size_t)pipeline->audio_block_samples * channels;
        memset(
            pipeline->pending_pcm + pipeline->pending_values,
            0,
            (flush_values - pipeline->pending_values) * sizeof(int16_t));
        const h2_pal_result_t result = write_audio_block(
            pipeline->track,
            pipeline->pending_pcm,
            pipeline->audio_block_samples,
            pipeline->audio_sample_rate_hz,
            channels,
            pipeline->config->acquire_timeout_ms);
        if (result != H2_PAL_OK) {
            pipeline_fail(pipeline, "audio-flush", result);
        }
        pipeline->pending_values = 0u;
    }
}

h2_pal_result_t h2_smoke_mp4_player_run(
    h2_runtime_t *runtime,
    const h2_smoke_mp4_player_config_t *config) {
    if (runtime == NULL || config == NULL || runtime->mem == NULL ||
        runtime->video_decoder == NULL || runtime->display == NULL ||
        runtime->time == NULL || runtime->task == NULL ||
        runtime->queue == NULL || runtime->sync == NULL ||
        (config->require_audio &&
         (runtime->audio_decoder == NULL || runtime->audio == NULL)) ||
        (config->display_mode != H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT &&
         config->display_mode != H2_SMOKE_MP4_PLAYER_DISPLAY_CENTER)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_mp4_decoder_source_api_t source = config->source;
    player_fs_source_t fs_source = {0};
    int close_source = 0;
    int display_opened = 0;
    h2_pal_result_t result = H2_PAL_OK;
    if (source.read_at == NULL) {
        if (config->media_path == NULL || config->media_path[0] == '\0' ||
            runtime->fs == NULL) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        h2_pal_fs_stat_t stat = {0};
        result = (h2_pal_result_t)h2_pal_fs_stat(
            runtime->fs, config->media_path, &stat);
        if (result != H2_PAL_OK || stat.is_dir || stat.size == 0u) {
            return player_fail(
                runtime,
                "source-stat",
                result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result);
        }
        result = (h2_pal_result_t)h2_pal_fs_open(
            runtime->fs,
            config->media_path,
            H2_PAL_FS_OPEN_READ,
            &fs_source.file);
        if (result != H2_PAL_OK) {
            return player_fail(runtime, "source-open", result);
        }
        fs_source.fs = runtime->fs;
        source = (h2_mp4_decoder_source_api_t){
            .user = &fs_source,
            .size = stat.size,
            .read_at = player_fs_read_at,
        };
        close_source = 1;
    } else if (source.size == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_mp4_decoder_config_t decoder_config = {
        .allocator = runtime->mem,
        .source = source,
        .video_decoder = *runtime->video_decoder,
        .audio_decoder = config->require_audio
            ? *runtime->audio_decoder
            : (h2_pal_audio_decoder_api_t){0},
        .video_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
        .require_video = 1,
        .require_audio = config->require_audio,
    };
    h2_mp4_decoder_t *decoder = NULL;
    result = h2_mp4_decoder_open(&decoder_config, &decoder);
    if (result != H2_PAL_OK) {
        result = player_fail(runtime, "decoder-open", result);
        goto close_media_source;
    }
    h2_mp4_decoder_info_t media = {0};
    result = h2_mp4_decoder_get_info(decoder, &media);
    if (result != H2_PAL_OK || !media.has_video ||
        media.width == 0u || media.height == 0u) {
        result = result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result;
        goto close_decoder;
    }
    if (media.has_audio &&
        (media.audio_channels == 0u ||
         media.audio_channels > H2_MP4_PLAYER_MAX_AUDIO_CHANNELS)) {
        result = H2_PAL_ERR_UNSUPPORTED;
        goto close_decoder;
    }
    {
        char message[H2_PAL_LOG_MESSAGE_MAX];
        (void)snprintf(
            message,
            sizeof(message),
            "H2_MP4_PLAYER_MEDIA video=%" PRIu32 "x%" PRIu32
            " audio=%u rate=%" PRIu32 " channels=%u",
            media.width,
            media.height,
            media.has_audio,
            media.audio_sample_rate_hz,
            media.audio_channels);
        player_log(runtime, H2_PAL_LOG_INFO, message);
    }

    int16_t *pending_pcm = NULL;
    uint16_t audio_block_samples = 0u;
    h2_pal_audio_track_t *track = NULL;
    int speaker_started = 0;
    if (config->require_audio && media.has_audio) {
        h2_audio_info_t audio_info = {0};
        result = (h2_pal_result_t)h2_pal_audio_get_info(
            runtime->audio, &audio_info);
        if (result == H2_PAL_OK &&
            (!audio_info.available || !audio_info.playback_supported ||
             audio_info.playback_format.sample_rate_hz !=
                 media.audio_sample_rate_hz ||
             audio_info.playback_format.channels != media.audio_channels ||
             audio_info.playback_format.sample_format !=
                 H2_AUDIO_SAMPLE_S16LE ||
             audio_info.playback_format.frame_samples_per_channel == 0u)) {
            result = H2_PAL_ERR_UNSUPPORTED;
        }
        if (result != H2_PAL_OK) {
            result = player_fail(runtime, "audio-info", result);
            goto close_audio;
        }
        audio_block_samples =
            audio_info.playback_format.frame_samples_per_channel;
        if ((size_t)audio_block_samples >
            SIZE_MAX / media.audio_channels / sizeof(int16_t)) {
            result = player_fail(
                runtime, "audio-buffer-size", H2_PAL_ERR_NO_MEMORY);
            goto close_audio;
        }
        pending_pcm = h2_pal_mem_alloc(
            runtime->mem,
            (size_t)audio_block_samples * media.audio_channels *
                sizeof(int16_t));
        if (pending_pcm == NULL) {
            result = player_fail(
                runtime, "audio-buffer", H2_PAL_ERR_NO_MEMORY);
            goto close_audio;
        }
        player_log(
            runtime, H2_PAL_LOG_INFO, "H2_MP4_PLAYER_STAGE audio-buffer");
        const size_t half_second_block_values =
            2u * (size_t)audio_block_samples;
        size_t audio_buffer_frames =
            media.audio_sample_rate_hz / half_second_block_values;
        if (media.audio_sample_rate_hz % half_second_block_values != 0u) {
            ++audio_buffer_frames;
        }
        if (audio_buffer_frames < 4u) {
            audio_buffer_frames = 4u;
        }
        const h2_audio_track_config_t track_config = {
            .name = "mp4-player",
            .format = {
                .sample_rate_hz = media.audio_sample_rate_hz,
                .frame_samples_per_channel = audio_block_samples,
                .channels = media.audio_channels,
                .sample_format = H2_AUDIO_SAMPLE_S16LE,
            },
            .volume_factor_milli = 1000u,
            .buffer_frames = audio_buffer_frames,
        };
        result = (h2_pal_result_t)h2_pal_audio_start_speaker(runtime->audio);
        if (result == H2_PAL_OK) {
            speaker_started = 1;
            result = (h2_pal_result_t)h2_pal_audio_create_track(
                runtime->audio, &track_config, &track);
        }
        if (result != H2_PAL_OK) {
            result = player_fail(runtime, "audio-open", result);
            goto close_audio;
        }
        player_log(
            runtime, H2_PAL_LOG_INFO, "H2_MP4_PLAYER_STAGE audio-open");
    }

    result = (h2_pal_result_t)h2_pal_display_open(runtime->display);
    if (result != H2_PAL_OK) {
        result = player_fail(runtime, "display-open", result);
        goto close_audio;
    }
    display_opened = 1;
    player_log(
        runtime, H2_PAL_LOG_INFO, "H2_MP4_PLAYER_STAGE display-open");
    h2_display_info_t display = {0};
    result = (h2_pal_result_t)h2_pal_display_get_info(runtime->display, &display);
    if (result != H2_PAL_OK || display.width <= 0 || display.height <= 0 ||
        media.width > (uint32_t)display.width ||
        media.height > (uint32_t)display.height ||
        (config->display_mode == H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT &&
         (media.width != (uint32_t)display.width ||
          media.height != (uint32_t)display.height))) {
        result = result == H2_PAL_OK ? H2_PAL_ERR_FORMAT : result;
        goto close_audio;
    }
    const h2_display_rect_t video_rect = {
        .x = (display.width - (int)media.width) / 2,
        .y = (display.height - (int)media.height) / 2,
        .width = (int)media.width,
        .height = (int)media.height,
    };
    if (config->display_mode == H2_SMOKE_MP4_PLAYER_DISPLAY_CENTER &&
        (video_rect.width != display.width ||
         video_rect.height != display.height)) {
        result = clear_display_black(runtime, &display);
        if (result != H2_PAL_OK) {
            result = player_fail(runtime, "display-clear", result);
            goto close_audio;
        }
    }

    player_pipeline_t pipeline = {
        .runtime = runtime,
        .config = config,
        .decoder = decoder,
        .track = track,
        .display = display,
        .video_rect = video_rect,
        .audio_block_samples = audio_block_samples,
        .audio_sample_rate_hz = media.audio_sample_rate_hz,
        .audio_channels = media.audio_channels,
        .pending_pcm = pending_pcm,
    };
    atomic_init(&pipeline.result, H2_PAL_OK);
    atomic_init(&pipeline.stop, 0);
    atomic_init(&pipeline.frame_count, 0u);
    const h2_pal_mutex_config_t buffer_mutex_config = {
        .name = "mp4-buffer-ref",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    result = h2_pal_mutex_create(
        runtime->sync, &buffer_mutex_config, &pipeline.buffer_mutex);
    if (result != H2_PAL_OK) {
        result = player_fail(runtime, "buffer-mutex", result);
        goto close_pipeline;
    }
    const size_t video_stride_bytes =
        (size_t)video_rect.width * sizeof(uint16_t);
    if ((size_t)video_rect.height > SIZE_MAX / video_stride_bytes) {
        result = player_fail(
            runtime, "video-buffer-size", H2_PAL_ERR_NO_MEMORY);
        goto close_pipeline;
    }
    const size_t video_bytes =
        video_stride_bytes * (size_t)video_rect.height;
    for (size_t i = 0u; i < H2_MP4_PLAYER_VIDEO_BUFFER_COUNT; ++i) {
        pipeline.buffers[i].video =
            h2_pal_mem_alloc(runtime->mem, video_bytes);
        if (pipeline.buffers[i].video == NULL) {
            result = player_fail(
                runtime, "video-buffer", H2_PAL_ERR_NO_MEMORY);
            goto close_pipeline;
        }
        pipeline.buffers[i].video_capacity = video_bytes;
        pipeline.buffers[i].video_stride_bytes = video_stride_bytes;
        pipeline.buffers[i].consumers = 0;
    }
    player_log(
        runtime, H2_PAL_LOG_INFO, "H2_MP4_PLAYER_STAGE video-buffers");
    const h2_pal_queue_config_t free_queue_config = {
        .name = "mp4-free-video",
        .item_size = sizeof(size_t),
        .item_count = H2_MP4_PLAYER_VIDEO_BUFFER_COUNT,
        .allocator = runtime->mem,
    };
    const h2_pal_queue_config_t ready_video_queue_config = {
        .name = "mp4-ready-video",
        .item_size = sizeof(size_t),
        .item_count = H2_MP4_PLAYER_VIDEO_BUFFER_COUNT,
        .allocator = runtime->mem,
    };
    result = (h2_pal_result_t)h2_pal_queue_create(
        runtime->queue,
        &free_queue_config,
        &pipeline.free_video);
    if (result == H2_PAL_OK) {
        result = (h2_pal_result_t)h2_pal_queue_create(
            runtime->queue,
            &ready_video_queue_config,
            &pipeline.ready_video);
    }
    if (result == H2_PAL_OK && track != NULL) {
        const h2_pal_queue_config_t ready_audio_queue_config = {
            .name = "mp4-ready-audio",
            .item_size = sizeof(size_t),
            .item_count = H2_MP4_PLAYER_VIDEO_BUFFER_COUNT,
            .allocator = runtime->mem,
        };
        result = (h2_pal_result_t)h2_pal_queue_create(
            runtime->queue,
            &ready_audio_queue_config,
            &pipeline.ready_audio);
    }
    if (result != H2_PAL_OK) {
        result = player_fail(runtime, "pipeline-queue", result);
        goto close_pipeline;
    }
    player_log(
        runtime, H2_PAL_LOG_INFO, "H2_MP4_PLAYER_STAGE pipeline-queues");
    for (size_t i = 0u; i < H2_MP4_PLAYER_VIDEO_BUFFER_COUNT; ++i) {
        result = (h2_pal_result_t)h2_pal_queue_send(
            runtime->queue,
            pipeline.free_video,
            &i,
            H2_PAL_QUEUE_NO_WAIT);
        if (result != H2_PAL_OK) {
            result = player_fail(runtime, "pipeline-prime", result);
            goto close_pipeline;
        }
    }

    h2_pal_task_t *decoder_task_handle = NULL;
    h2_pal_task_t *audio_task_handle = NULL;
    const h2_pal_task_options_t audio_task_options = {
        .name = h2_smoke_mp4_player_audio_task_name,
        .min_stack_size = 8192u,
    };
    const h2_pal_task_options_t decoder_task_options = {
        .name = h2_smoke_mp4_player_decoder_task_name,
        .min_stack_size = H2_MP4_PLAYER_DECODER_TASK_STACK_BYTES,
    };
    player_log(runtime, H2_PAL_LOG_INFO, "H2_MP4_PLAYER_OPEN");
    if (track != NULL) {
        player_log(
            runtime, H2_PAL_LOG_INFO,
            "H2_MP4_PLAYER_STAGE audio-task-start-before");
        result = h2_pal_task_start(
            runtime->task,
            &audio_task_options,
            audio_writer_task,
            &pipeline,
            &audio_task_handle);
        player_log(
            runtime,
            result == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
            result == H2_PAL_OK
                ? "H2_MP4_PLAYER_STAGE audio-task-start-after"
                : "H2_MP4_PLAYER_FAIL stage=audio-task-start");
    }
    if (result == H2_PAL_OK) {
        player_log(
            runtime, H2_PAL_LOG_INFO,
            "H2_MP4_PLAYER_STAGE decoder-task-start-before");
        result = h2_pal_task_start(
            runtime->task,
            &decoder_task_options,
            decoder_task,
            &pipeline,
            &decoder_task_handle);
        player_log(
            runtime,
            result == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
            result == H2_PAL_OK
                ? "H2_MP4_PLAYER_STAGE decoder-task-start-after"
                : "H2_MP4_PLAYER_FAIL stage=decoder-task-start");
    }
    if (result == H2_PAL_OK) {
        player_log(
            runtime, H2_PAL_LOG_INFO,
            "H2_MP4_PLAYER_STAGE video-writer-enter");
        video_writer_task(&pipeline);
    }
    if (result != H2_PAL_OK) {
        pipeline_fail(&pipeline, "pipeline-task", result);
        (void)h2_pal_queue_close(runtime->queue, pipeline.ready_video);
        if (pipeline.ready_audio != NULL) {
            (void)h2_pal_queue_close(runtime->queue, pipeline.ready_audio);
        }
    }
    if (decoder_task_handle != NULL) {
        const h2_pal_result_t join_result =
            h2_pal_task_join(runtime->task, decoder_task_handle);
        decoder_task_handle = NULL;
        if (join_result != H2_PAL_OK) {
            pipeline_fail(&pipeline, "decoder-join", join_result);
        }
    }
    (void)h2_pal_queue_close(runtime->queue, pipeline.ready_video);
    if (pipeline.ready_audio != NULL) {
        (void)h2_pal_queue_close(runtime->queue, pipeline.ready_audio);
    }
    if (audio_task_handle != NULL) {
        const h2_pal_result_t join_result =
            h2_pal_task_join(runtime->task, audio_task_handle);
        audio_task_handle = NULL;
        if (join_result != H2_PAL_OK) {
            pipeline_fail(&pipeline, "audio-join", join_result);
        }
    }
    result = (h2_pal_result_t)atomic_load(&pipeline.result);

close_pipeline:
    if (pipeline.free_video != NULL) {
        h2_pal_queue_destroy(runtime->queue, pipeline.free_video);
    }
    if (pipeline.ready_video != NULL) {
        h2_pal_queue_destroy(runtime->queue, pipeline.ready_video);
    }
    if (pipeline.ready_audio != NULL) {
        h2_pal_queue_destroy(runtime->queue, pipeline.ready_audio);
    }
    for (size_t i = 0u; i < H2_MP4_PLAYER_VIDEO_BUFFER_COUNT; ++i) {
        h2_pal_mem_free(runtime->mem, pipeline.buffers[i].pcm);
        h2_pal_mem_free(runtime->mem, pipeline.buffers[i].video);
    }
    if (pipeline.buffer_mutex != NULL) {
        (void)h2_pal_mutex_destroy(runtime->sync, pipeline.buffer_mutex);
    }
close_audio:
    if (track != NULL) {
        const h2_pal_result_t close_result =
            (h2_pal_result_t)h2_pal_audio_track_close(track);
        if (result == H2_PAL_OK) {
            result = close_result;
        }
    }
    if (speaker_started) {
        const h2_pal_result_t stop_result =
            (h2_pal_result_t)h2_pal_audio_stop_speaker(runtime->audio);
        if (result == H2_PAL_OK) {
            result = stop_result;
        }
    }
    h2_pal_mem_free(runtime->mem, pending_pcm);
close_decoder:
    {
        const h2_pal_result_t close_result = h2_mp4_decoder_close(decoder);
        if (result == H2_PAL_OK) {
            result = close_result;
        }
    }
    if (display_opened) {
        const h2_pal_result_t close_result =
            (h2_pal_result_t)h2_pal_display_close(runtime->display);
        if (result == H2_PAL_OK) {
            result = close_result;
        }
    }
close_media_source:
    if (close_source) {
        const h2_pal_result_t close_result =
            (h2_pal_result_t)h2_pal_fs_close(fs_source.fs, fs_source.file);
        if (result == H2_PAL_OK) {
            result = close_result;
        }
    }
    return result;
}
