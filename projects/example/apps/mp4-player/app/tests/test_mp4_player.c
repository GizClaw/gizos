#include "h2_smoke_mp4_player.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct test_state test_state_t;

struct h2_pal_video_decoder_frame {
    struct h2_pal_video_decoder_session *owner;
};

struct h2_pal_video_decoder_session {
    h2_pal_mem_api_t allocator;
    uint16_t *pixels;
    size_t pixel_bytes;
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

struct h2_pal_fs_file {
    FILE *file;
};

struct h2_pal_queue {
    h2_pal_mem_api_t allocator;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    uint8_t *items;
    size_t item_size;
    size_t item_count;
    size_t head;
    size_t tail;
    size_t count;
    int closed;
};

struct h2_pal_task {
    pthread_t thread;
    h2_pal_task_entry_t entry;
    void *context;
};

struct test_state {
    size_t allocations;
    size_t draw_calls;
    size_t present_calls;
    size_t audio_writes;
    size_t silent_audio_writes;
    size_t nonzero_audio_writes;
    size_t audio_ready_logs;
    size_t ready_logs;
    size_t ready_callbacks;
    size_t fs_open_calls;
    size_t fs_close_calls;
    size_t loop_logs;
    size_t display_open_calls;
    size_t display_close_calls;
    size_t speaker_start_calls;
    size_t speaker_stop_calls;
    size_t decoder_task_starts;
    size_t audio_task_starts;
    size_t stop_calls;
    pthread_t caller_thread;
    size_t sleep_calls;
    uint64_t now_ms;
    uint64_t slept_ms;
    int display_width;
    int display_height;
    h2_display_rect_t last_draw_rect;
    int64_t audio_pts_offset_us;
    int16_t last_audio_sample;
    const char *fixture_path;
    h2_pal_fs_file_t fs_file;
    h2_pal_audio_track_t track;
};

static void *tracked_alloc(void *user, size_t size) {
    test_state_t *state = user;
    void *pointer = malloc(size);
    if (pointer != NULL) ++state->allocations;
    return pointer;
}

static void tracked_free(void *user, void *pointer) {
    test_state_t *state = user;
    if (pointer != NULL) --state->allocations;
    free(pointer);
}

static void queue_deadline(uint32_t timeout_ms, struct timespec *deadline) {
    assert(timespec_get(deadline, TIME_UTC) == TIME_UTC);
    deadline->tv_sec += (time_t)(timeout_ms / 1000u);
    deadline->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

static int queue_create(
    void *user,
    const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
    (void)user;
    h2_pal_queue_t *queue =
        h2_pal_mem_alloc(config->allocator, sizeof(*queue));
    if (queue == NULL ||
        config->item_count > SIZE_MAX / config->item_size) {
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(queue, 0, sizeof(*queue));
    queue->allocator = *config->allocator;
    queue->items = h2_pal_mem_alloc(
        config->allocator,
        config->item_count * config->item_size);
    if (queue->items == NULL) {
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (pthread_cond_init(&queue->changed, NULL) != 0) {
        assert(pthread_mutex_destroy(&queue->mutex) == 0);
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    *out_queue = queue;
    return H2_PAL_OK;
}

static void queue_destroy(void *user, h2_pal_queue_t *queue) {
    (void)user;
    assert(queue != NULL);
    const h2_pal_mem_api_t allocator = queue->allocator;
    assert(pthread_cond_destroy(&queue->changed) == 0);
    assert(pthread_mutex_destroy(&queue->mutex) == 0);
    h2_pal_mem_free(&allocator, queue->items);
    h2_pal_mem_free(&allocator, queue);
}

static int queue_wait(
    h2_pal_queue_t *queue,
    int need_space,
    uint32_t timeout_ms) {
    if (timeout_ms == H2_PAL_QUEUE_NO_WAIT) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (timeout_ms == H2_PAL_QUEUE_WAIT_FOREVER) {
        while (!queue->closed &&
               (need_space
                    ? queue->count == queue->item_count
                    : queue->count == 0u)) {
            assert(pthread_cond_wait(&queue->changed, &queue->mutex) == 0);
        }
        return queue->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
    }
    struct timespec deadline = {0};
    queue_deadline(timeout_ms, &deadline);
    while (!queue->closed &&
           (need_space
                ? queue->count == queue->item_count
                : queue->count == 0u)) {
        const int result =
            pthread_cond_timedwait(
                &queue->changed,
                &queue->mutex,
                &deadline);
        if (result == ETIMEDOUT) {
            return H2_PAL_ERR_TIMEOUT;
        }
        assert(result == 0);
    }
    return queue->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
}

static int queue_send(
    void *user,
    h2_pal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms) {
    (void)user;
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    int result = H2_PAL_OK;
    if (queue->closed) {
        result = H2_PAL_ERR_CLOSED;
    } else if (queue->count == queue->item_count) {
        result = queue_wait(queue, 1, timeout_ms);
    }
    if (result == H2_PAL_OK) {
        memcpy(
            queue->items + queue->tail * queue->item_size,
            item,
            queue->item_size);
        queue->tail = (queue->tail + 1u) % queue->item_count;
        ++queue->count;
        assert(pthread_cond_broadcast(&queue->changed) == 0);
    }
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return result;
}

static int queue_send_latest(
    void *user,
    h2_pal_queue_t *queue,
    const void *item) {
    (void)user;
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    int result = H2_PAL_OK;
    if (queue->closed) {
        result = H2_PAL_ERR_CLOSED;
    } else {
        if (queue->count == queue->item_count) {
            queue->head = (queue->head + 1u) % queue->item_count;
            --queue->count;
        }
        memcpy(
            queue->items + queue->tail * queue->item_size,
            item,
            queue->item_size);
        queue->tail = (queue->tail + 1u) % queue->item_count;
        ++queue->count;
        assert(pthread_cond_broadcast(&queue->changed) == 0);
    }
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return result;
}

static int queue_recv(
    void *user,
    h2_pal_queue_t *queue,
    void *out_item,
    uint32_t timeout_ms) {
    (void)user;
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    int result = H2_PAL_OK;
    if (queue->count == 0u) {
        result = queue_wait(queue, 0, timeout_ms);
    }
    if (result == H2_PAL_OK) {
        memcpy(
            out_item,
            queue->items + queue->head * queue->item_size,
            queue->item_size);
        queue->head = (queue->head + 1u) % queue->item_count;
        --queue->count;
        assert(pthread_cond_broadcast(&queue->changed) == 0);
    }
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return result;
}

static int queue_reset(void *user, h2_pal_queue_t *queue) {
    (void)user;
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    queue->head = 0u;
    queue->tail = 0u;
    queue->count = 0u;
    assert(pthread_cond_broadcast(&queue->changed) == 0);
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return H2_PAL_OK;
}

static int queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    queue->closed = 1;
    assert(pthread_cond_broadcast(&queue->changed) == 0);
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return H2_PAL_OK;
}

static const h2_pal_queue_vtable_t s_queue_vtable = {
    .create = queue_create,
    .destroy = queue_destroy,
    .send = queue_send,
    .send_latest = queue_send_latest,
    .recv = queue_recv,
    .reset = queue_reset,
    .close = queue_close,
};

static void *task_trampoline(void *context) {
    h2_pal_task_t *task = context;
    task->entry(task->context);
    return NULL;
}

static int task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *context,
    h2_pal_task_t **out_task) {
    test_state_t *state = user;
    if (strcmp(options->name, "mp4-decoder") == 0) {
        ++state->decoder_task_starts;
    } else if (strcmp(options->name, "mp4-audio-writer") == 0) {
        ++state->audio_task_starts;
    } else {
        assert(0);
    }
    h2_pal_task_t *task = malloc(sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    task->entry = entry;
    task->context = context;
    if (pthread_create(&task->thread, NULL, task_trampoline, task) != 0) {
        free(task);
        return H2_PAL_ERR_TASK;
    }
    *out_task = task;
    return H2_PAL_OK;
}

static int task_join(void *user, h2_pal_task_t *task) {
    (void)user;
    if (pthread_join(task->thread, NULL) != 0) {
        return H2_PAL_ERR_TASK;
    }
    free(task);
    return H2_PAL_OK;
}

static const h2_pal_task_vtable_t s_task_vtable = {
    .start = task_start,
    .join = task_join,
};

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

static h2_pal_result_t video_open(
    void *user,
    const h2_video_decoder_config_t *config,
    h2_pal_video_decoder_session_t **out_session) {
    (void)user;
    h2_pal_video_decoder_session_t *session =
        h2_pal_mem_alloc(config->frame_allocator, sizeof(*session));
    if (session == NULL) return H2_PAL_ERR_NO_MEMORY;
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
    (void)user;
    if (session->configured || session->acquired) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const size_t pixels = (size_t)config->visible_width * config->visible_height;
    if (pixels > SIZE_MAX / sizeof(uint16_t)) return H2_PAL_ERR_NO_MEMORY;
    session->pixel_bytes = pixels * sizeof(uint16_t);
    session->pixels =
        h2_pal_mem_alloc(&session->allocator, session->pixel_bytes);
    if (session->pixels == NULL) return H2_PAL_ERR_NO_MEMORY;
    for (size_t i = 0u; i < pixels; ++i) {
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
    (void)user;
    if (!session->configured || session->eos) return H2_PAL_ERR_INVALID_STATE;
    if ((packet->flags & H2_VIDEO_DECODER_PACKET_END_OF_STREAM) != 0u) {
        session->eos = 1;
        return H2_PAL_OK;
    }
    if (session->ready || session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    session->pts_us = packet->pts_us;
    session->duration_us = packet->duration_us;
    session->ready = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t video_acquire(
    void *user,
    h2_pal_video_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_video_decoder_frame_t **out_frame) {
    (void)user;
    (void)timeout_ms;
    if (session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
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
            .stride_bytes = (size_t)session->width * sizeof(uint16_t),
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
    (void)user;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    h2_pal_mem_free(&session->allocator, session->pixels);
    session->pixels = NULL;
    session->configured = 0;
    session->ready = 0;
    session->eos = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t video_close(
    void *user,
    h2_pal_video_decoder_session_t *session) {
    (void)user;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    const h2_pal_mem_api_t allocator = session->allocator;
    h2_pal_mem_free(&allocator, session->pixels);
    h2_pal_mem_free(&allocator, session);
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

static h2_pal_result_t audio_decoder_open(
    void *user,
    const h2_audio_decoder_config_t *config,
    h2_pal_audio_decoder_session_t **out_session) {
    (void)user;
    h2_pal_audio_decoder_session_t *session =
        h2_pal_mem_alloc(config->pcm_allocator, sizeof(*session));
    if (session == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(session, 0, sizeof(*session));
    session->allocator = *config->pcm_allocator;
    session->frame.owner = session;
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_decoder_configure(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_stream_config_t *config) {
    (void)user;
    session->sample_bytes =
        1024u * config->channels * sizeof(int16_t);
    session->samples =
        h2_pal_mem_alloc(&session->allocator, session->sample_bytes);
    if (session->samples == NULL) return H2_PAL_ERR_NO_MEMORY;
    for (size_t i = 0u; i < session->sample_bytes / sizeof(int16_t); ++i) {
        session->samples[i] = (int16_t)(i + 1u);
    }
    session->sample_rate_hz = config->sample_rate_hz;
    session->channels = config->channels;
    session->configured = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_decoder_submit(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    const h2_audio_decoder_packet_t *packet) {
    test_state_t *state = user;
    if (!session->configured || session->eos) return H2_PAL_ERR_INVALID_STATE;
    if ((packet->flags & H2_AUDIO_DECODER_PACKET_END_OF_STREAM) != 0u) {
        session->eos = 1;
        return H2_PAL_OK;
    }
    if (session->ready || session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    assert(packet->pts_us <= INT64_MAX - state->audio_pts_offset_us);
    session->pts_us = packet->pts_us + state->audio_pts_offset_us;
    session->duration_us = packet->duration_us;
    session->ready = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_decoder_acquire(
    void *user,
    h2_pal_audio_decoder_session_t *session,
    uint32_t timeout_ms,
    h2_pal_audio_decoder_frame_t **out_frame) {
    (void)user;
    (void)timeout_ms;
    if (session->acquired) return H2_PAL_ERR_WOULD_BLOCK;
    if (!session->ready) {
        return session->eos ? H2_PAL_EXIT : H2_PAL_ERR_WOULD_BLOCK;
    }
    session->acquired = 1;
    *out_frame = &session->frame;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_decoder_info(
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

static h2_pal_result_t audio_decoder_release(
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

static h2_pal_result_t audio_decoder_reset(
    void *user,
    h2_pal_audio_decoder_session_t *session) {
    (void)user;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    h2_pal_mem_free(&session->allocator, session->samples);
    session->samples = NULL;
    session->configured = 0;
    session->ready = 0;
    session->eos = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t audio_decoder_close(
    void *user,
    h2_pal_audio_decoder_session_t *session) {
    (void)user;
    if (session->acquired) return H2_PAL_ERR_INVALID_STATE;
    const h2_pal_mem_api_t allocator = session->allocator;
    h2_pal_mem_free(&allocator, session->samples);
    h2_pal_mem_free(&allocator, session);
    return H2_PAL_OK;
}

static const h2_pal_audio_decoder_vtable_t s_audio_decoder_vtable = {
    .open = audio_decoder_open,
    .configure = audio_decoder_configure,
    .submit_packet = audio_decoder_submit,
    .acquire_frame = audio_decoder_acquire,
    .frame_get_info = audio_decoder_info,
    .release_frame = audio_decoder_release,
    .reset = audio_decoder_reset,
    .close = audio_decoder_close,
};

static int display_open(void *user) {
    ++((test_state_t *)user)->display_open_calls;
    return H2_PAL_OK;
}

static int display_get_info(void *user, h2_display_info_t *out_info) {
    test_state_t *state = user;
    *out_info = (h2_display_info_t){
        .width = state->display_width == 0 ? 160 : state->display_width,
        .height = state->display_height == 0 ? 96 : state->display_height,
        .native_format = H2_DISPLAY_PIXEL_RGB565,
    };
    return H2_PAL_OK;
}

static int display_draw(
    void *user,
    const h2_display_rect_t *rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format) {
    test_state_t *state = user;
    assert(rect->width > 0 && rect->height > 0);
    assert(pixels != NULL);
    assert(stride_bytes == (size_t)rect->width * sizeof(uint16_t));
    assert(format == H2_DISPLAY_PIXEL_RGB565);
    state->last_draw_rect = *rect;
    ++state->draw_calls;
    return H2_PAL_OK;
}

static int display_present(void *user) {
    ++((test_state_t *)user)->present_calls;
    return H2_PAL_OK;
}

static int display_close(void *user) {
    ++((test_state_t *)user)->display_close_calls;
    return H2_PAL_OK;
}

static const h2_pal_display_vtable_t s_display_vtable = {
    .open = display_open,
    .get_info = display_get_info,
    .draw_bitmap = display_draw,
    .present = display_present,
    .close = display_close,
};

static int track_write(
    h2_pal_audio_track_t *track,
    const h2_audio_frame_t *frame,
    uint32_t timeout_ms) {
    (void)timeout_ms;
    test_state_t *state = track->user;
    assert(frame->sample_rate_hz == 16000u);
    assert(frame->samples_per_channel == 256u && frame->channels == 1u);
    assert(frame->bytes == 256u * sizeof(int16_t));
    const int16_t *samples = frame->data;
    int nonzero = 0;
    for (size_t i = 0u; i < frame->samples_per_channel; ++i) {
        nonzero |= samples[i] != 0;
    }
    state->silent_audio_writes += !nonzero;
    state->nonzero_audio_writes += nonzero;
    state->last_audio_sample = samples[frame->samples_per_channel - 1u];
    ++state->audio_writes;
    return H2_PAL_OK;
}

static int track_close(h2_pal_audio_track_t *track) {
    assert(track != NULL);
    return H2_PAL_OK;
}

static int audio_start_speaker(void *user) {
    ++((test_state_t *)user)->speaker_start_calls;
    return H2_PAL_OK;
}

static int audio_get_info(void *user, h2_audio_info_t *info) {
    (void)user;
    *info = (h2_audio_info_t){
        .available = 1,
        .playback_supported = 1,
        .playback_format = {
            .sample_rate_hz = 16000u,
            .frame_samples_per_channel = 256u,
            .channels = 1u,
            .sample_format = H2_AUDIO_SAMPLE_S16LE,
        },
    };
    return H2_PAL_OK;
}

static int audio_stop_speaker(void *user) {
    ++((test_state_t *)user)->speaker_stop_calls;
    return H2_PAL_OK;
}

static int audio_create_track(
    void *user,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
    test_state_t *state = user;
    assert(config->format.sample_rate_hz == 16000u);
    assert(config->format.frame_samples_per_channel == 256u);
    assert(config->format.channels == 1u);
    assert(config->buffer_frames == 32u);
    state->track = (h2_pal_audio_track_t){
        .user = state,
        .write = track_write,
        .close = track_close,
    };
    *out_track = &state->track;
    return H2_PAL_OK;
}

static const h2_pal_audio_vtable_t s_audio_vtable = {
    .get_info = audio_get_info,
    .start_speaker = audio_start_speaker,
    .stop_speaker = audio_stop_speaker,
    .create_track = audio_create_track,
};

static h2_pal_result_t time_monotonic(void *user, uint64_t *out_ms) {
    test_state_t *state = user;
    *out_ms = state->now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t time_sleep(void *user, uint32_t delay_ms) {
    test_state_t *state = user;
    assert(state->now_ms <= UINT64_MAX - delay_ms);
    state->now_ms += delay_ms;
    state->slept_ms += delay_ms;
    ++state->sleep_calls;
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t s_time_vtable = {
    .get_monotonic_ms = time_monotonic,
    .sleep_ms = time_sleep,
};

static int log_write(
    void *user,
    h2_pal_log_level_t level,
    const char *scope,
    const char *message) {
    (void)level;
    (void)scope;
    test_state_t *state = user;
    state->audio_ready_logs +=
        strcmp(message, "H2_MP4_PLAYER_AUDIO_READY") == 0;
    state->ready_logs += strcmp(message, "H2_MP4_PLAYER_READY") == 0;
    state->loop_logs += strcmp(message, "H2_MP4_PLAYER_LOOP") == 0;
    return H2_PAL_OK;
}

static const h2_pal_log_vtable_t s_log_vtable = {.write = log_write};

static h2_pal_result_t ready_callback(void *user) {
    ++((test_state_t *)user)->ready_callbacks;
    return H2_PAL_OK;
}

static int stop_callback(void *user) {
    test_state_t *state = user;
    assert(pthread_equal(pthread_self(), state->caller_thread) != 0);
    ++state->stop_calls;
    return 1;
}

static int fs_open(
    void *user,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    test_state_t *state = user;
    if (strcmp(path, "/fixture.mp4") != 0 || mode != H2_PAL_FS_OPEN_READ) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    state->fs_file.file = fopen(state->fixture_path, "rb");
    if (state->fs_file.file == NULL) {
        return H2_PAL_ERR_IO;
    }
    ++state->fs_open_calls;
    *out_file = &state->fs_file;
    return H2_PAL_OK;
}

static int fs_read(
    void *user,
    h2_pal_fs_file_t *file,
    void *data,
    size_t len,
    size_t *out_read) {
    (void)user;
    *out_read = fread(data, 1u, len, file->file);
    return *out_read == len || feof(file->file) ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int fs_close(void *user, h2_pal_fs_file_t *file) {
    test_state_t *state = user;
    const int result = fclose(file->file);
    file->file = NULL;
    ++state->fs_close_calls;
    return result == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int fs_stat(
    void *user,
    const char *path,
    h2_pal_fs_stat_t *out_stat) {
    test_state_t *state = user;
    if (strcmp(path, "/fixture.mp4") != 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    FILE *file = fopen(state->fixture_path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return H2_PAL_ERR_IO;
    }
    const long size = ftell(file);
    fclose(file);
    if (size <= 0) {
        return H2_PAL_ERR_IO;
    }
    *out_stat = (h2_pal_fs_stat_t){
        .size = (uint64_t)size,
        .is_dir = 0,
    };
    return H2_PAL_OK;
}

static const h2_pal_fs_vtable_t s_fs_vtable = {
    .open = fs_open,
    .read = fs_read,
    .close = fs_close,
    .stat = fs_stat,
};

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *fixture_path = argv[1];
    assert(h2_smoke_mp4_player_run(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);

    FILE *file = fopen(fixture_path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    const long file_size = ftell(file);
    assert(file_size > 0);

    test_state_t state = {
        .audio_pts_offset_us = 500000,
        .fixture_path = fixture_path,
        .caller_thread = pthread_self(),
    };
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = tracked_alloc,
        .free = tracked_free,
    };
    const h2_pal_mem_api_t mem = {.user = &state, .vtable = &mem_vtable};
    const h2_pal_video_decoder_api_t video_decoder = {
        .vtable = &s_video_vtable,
    };
    const h2_pal_audio_decoder_api_t audio_decoder = {
        .user = &state,
        .vtable = &s_audio_decoder_vtable,
    };
    const h2_pal_display_api_t display = {
        .user = &state,
        .vtable = &s_display_vtable,
    };
    const h2_pal_audio_api_t audio = {
        .user = &state,
        .vtable = &s_audio_vtable,
    };
    const h2_pal_time_api_t time = {
        .user = &state,
        .vtable = &s_time_vtable,
    };
    const h2_pal_log_api_t log = {
        .user = &state,
        .vtable = &s_log_vtable,
    };
    const h2_pal_fs_api_t fs = {
        .user = &state,
        .vtable = &s_fs_vtable,
    };
    const h2_pal_queue_api_t queue = {
        .vtable = &s_queue_vtable,
    };
    const h2_pal_task_api_t task = {
        .user = &state,
        .vtable = &s_task_vtable,
    };
    h2_runtime_t runtime = {
        .mem = &mem,
        .fs = &fs,
        .log = &log,
        .time = &time,
        .task = &task,
        .queue = &queue,
        .display = &display,
        .audio = &audio,
        .audio_decoder = &audio_decoder,
        .video_decoder = &video_decoder,
    };
    const h2_smoke_mp4_player_config_t config = {
        .source = {
            .user = file,
            .size = (uint64_t)file_size,
            .read_at = source_read_at,
        },
        .acquire_timeout_ms = 25u,
        .max_frames = 49u,
        .looping = 1,
        .require_audio = 1,
        .on_ready = ready_callback,
        .ready_user = &state,
    };
    assert(h2_smoke_mp4_player_run(&runtime, &config) == H2_PAL_OK);
    assert(state.draw_calls == 49u && state.present_calls == 49u);
    assert(state.audio_writes > 0u);
    assert(state.silent_audio_writes > 0u);
    assert(state.nonzero_audio_writes > 0u);
    assert(
        state.audio_writes ==
        state.silent_audio_writes + state.nonzero_audio_writes);
    assert(state.last_audio_sample == 0);
    assert(state.audio_ready_logs == 1u);
    assert(state.ready_logs == 1u && state.loop_logs == 1u);
    assert(state.ready_callbacks == 1u);
    assert(state.display_open_calls == 1u && state.display_close_calls == 1u);
    assert(state.speaker_start_calls == 1u && state.speaker_stop_calls == 1u);
    assert(state.decoder_task_starts == 1u);
    assert(state.audio_task_starts == 1u);
    assert(state.sleep_calls > 0u && state.slept_ms > 0u);
    assert(state.now_ms == state.slept_ms);
    assert(state.allocations == 0u);
    fclose(file);

    const h2_smoke_mp4_player_config_t fs_config = {
        .media_path = "/fixture.mp4",
        .acquire_timeout_ms = 25u,
        .max_frames = 1u,
        .require_audio = 1,
        .on_ready = ready_callback,
        .ready_user = &state,
    };
    assert(h2_smoke_mp4_player_run(&runtime, &fs_config) == H2_PAL_OK);
    assert(state.fs_open_calls == 1u && state.fs_close_calls == 1u);
    assert(state.ready_callbacks == 2u);
    assert(state.decoder_task_starts == 2u);
    assert(state.audio_task_starts == 2u);
    assert(state.allocations == 0u);

    const size_t audio_writes_before_video_only = state.audio_writes;
    runtime.audio = NULL;
    const h2_smoke_mp4_player_config_t video_only_config = {
        .media_path = "/fixture.mp4",
        .acquire_timeout_ms = 25u,
        .max_frames = 1u,
        .on_ready = ready_callback,
        .ready_user = &state,
    };
    assert(
        h2_smoke_mp4_player_run(&runtime, &video_only_config) == H2_PAL_OK);
    assert(state.fs_open_calls == 2u && state.fs_close_calls == 2u);
    assert(state.ready_callbacks == 3u);
    assert(state.decoder_task_starts == 3u);
    assert(state.audio_task_starts == 2u);
    assert(state.speaker_start_calls == 2u);
    assert(state.audio_writes == audio_writes_before_video_only);
    assert(state.allocations == 0u);

    const size_t draw_calls_before_center = state.draw_calls;
    const size_t present_calls_before_center = state.present_calls;
    state.display_width = 200;
    state.display_height = 120;
    const h2_smoke_mp4_player_config_t center_config = {
        .media_path = "/fixture.mp4",
        .acquire_timeout_ms = 25u,
        .max_frames = 1u,
        .display_mode = H2_SMOKE_MP4_PLAYER_DISPLAY_CENTER,
    };
    assert(h2_smoke_mp4_player_run(&runtime, &center_config) == H2_PAL_OK);
    assert(state.draw_calls == draw_calls_before_center + 2u);
    assert(state.present_calls == present_calls_before_center + 2u);
    assert(
        state.last_draw_rect.x == 20 &&
        state.last_draw_rect.y == 12 &&
        state.last_draw_rect.width == 160 &&
        state.last_draw_rect.height == 96);
    assert(state.allocations == 0u);
    state.display_width = 0;
    state.display_height = 0;

    const h2_smoke_mp4_player_config_t invalid_display_mode_config = {
        .media_path = "/fixture.mp4",
        .display_mode = (h2_smoke_mp4_player_display_mode_t)2,
    };
    assert(
        h2_smoke_mp4_player_run(&runtime, &invalid_display_mode_config) ==
        H2_PAL_ERR_INVALID_ARG);

    const h2_smoke_mp4_player_config_t stop_config = {
        .media_path = "/fixture.mp4",
        .acquire_timeout_ms = 25u,
        .should_stop = stop_callback,
        .stop_user = &state,
    };
    assert(h2_smoke_mp4_player_run(&runtime, &stop_config) == H2_PAL_OK);
    assert(state.fs_open_calls == 4u && state.fs_close_calls == 4u);
    assert(state.stop_calls == 1u);
    assert(state.decoder_task_starts == 5u);
    assert(state.audio_task_starts == 2u);
    assert(state.ready_callbacks == 3u);
    assert(state.allocations == 0u);
    return 0;
}
