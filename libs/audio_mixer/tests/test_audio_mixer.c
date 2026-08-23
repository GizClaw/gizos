#include "h2_audio_mixer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_QUEUE_COUNT 4u
#define TEST_QUEUE_ITEMS 8u
#define TEST_QUEUE_ITEM_BYTES 64u

typedef struct test_queue {
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
    uint8_t items[TEST_QUEUE_ITEMS][TEST_QUEUE_ITEM_BYTES];
    int used;
    int closed;
} test_queue_t;

struct h2_pal_mutex {
    int locked;
};

struct h2_pal_cond {
    int unused;
};

typedef struct test_env {
    h2_audio_mixer_t *mixer;
    test_queue_t queues[TEST_QUEUE_COUNT];
    test_queue_t *drain_queue;
    uint32_t mixed_frames;
    int16_t first_mixed[4];
    struct h2_pal_mutex mutex;
    struct h2_pal_cond cond;
} test_env_t;

static h2_pal_result_t test_mutex_create(void *user, const h2_pal_mutex_config_t *config,
                             h2_pal_mutex_t **out_mutex) {
    test_env_t *env = user;
    if (config == NULL || out_mutex == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    env->mutex.locked = 0;
    *out_mutex = &env->mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(mutex != NULL && !mutex->locked);
    return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(mutex != NULL && !mutex->locked);
    mutex->locked = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t test_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(mutex != NULL && mutex->locked);
    mutex->locked = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t test_cond_create(void *user, const h2_pal_cond_config_t *config,
                            h2_pal_cond_t **out_cond) {
    test_env_t *env = user;
    if (config == NULL || out_cond == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_cond = &env->cond;
    return H2_PAL_OK;
}

static h2_pal_result_t test_cond_destroy(void *user, h2_pal_cond_t *cond) {
    (void)user;
    assert(cond != NULL);
    return H2_PAL_OK;
}

static h2_pal_result_t test_cond_wait(void *user, h2_pal_cond_t *cond,
                          h2_pal_mutex_t *mutex, uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    assert(cond != NULL && mutex != NULL && mutex->locked);
    return H2_PAL_OK;
}

static h2_pal_result_t test_cond_broadcast(void *user, h2_pal_cond_t *cond) {
    (void)user;
    assert(cond != NULL);
    return H2_PAL_OK;
}

static int test_queue_create(void *user, const h2_pal_queue_config_t *config,
                             h2_pal_queue_t **out_queue) {
    test_env_t *env = user;
    if (config == NULL || out_queue == NULL || config->item_size == 0u ||
        config->item_size > TEST_QUEUE_ITEM_BYTES || config->item_count == 0u ||
        config->item_count > TEST_QUEUE_ITEMS) {
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < TEST_QUEUE_COUNT; ++index) {
        test_queue_t *queue = &env->queues[index];
        if (queue->used)
            continue;
        memset(queue, 0, sizeof(*queue));
        queue->used = 1;
        queue->item_size = config->item_size;
        queue->capacity = config->item_count;
        if (config->item_size == sizeof(uint64_t))
            env->drain_queue = queue;
        *out_queue = (h2_pal_queue_t *)queue;
        return H2_PAL_QUEUE_OK;
    }
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
}

static void test_queue_destroy(void *user, h2_pal_queue_t *opaque) {
    test_env_t *env = user;
    test_queue_t *queue = (test_queue_t *)opaque;
    if (env->drain_queue == queue)
        env->drain_queue = NULL;
    memset(queue, 0, sizeof(*queue));
}

static int test_queue_send(void *user, h2_pal_queue_t *opaque,
                           const void *item, uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    test_queue_t *queue = (test_queue_t *)opaque;
    if (queue == NULL || item == NULL)
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    if (queue->closed)
        return H2_PAL_QUEUE_ERR_CLOSED;
    if (queue->count == queue->capacity)
        return H2_PAL_QUEUE_ERR_TIMEOUT;
    const size_t tail = (queue->head + queue->count) % queue->capacity;
    memcpy(queue->items[tail], item, queue->item_size);
    ++queue->count;
    return H2_PAL_QUEUE_OK;
}

static int test_queue_send_latest(void *user, h2_pal_queue_t *opaque,
                                  const void *item) {
    test_queue_t *queue = (test_queue_t *)opaque;
    if (queue != NULL && queue->count == queue->capacity) {
        queue->head = (queue->head + 1u) % queue->capacity;
        --queue->count;
    }
    return test_queue_send(user, opaque, item, H2_PAL_QUEUE_NO_WAIT);
}

static int test_queue_recv(void *user, h2_pal_queue_t *opaque, void *out_item,
                           uint32_t timeout_ms) {
    (void)timeout_ms;
    test_env_t *env = user;
    test_queue_t *queue = (test_queue_t *)opaque;
    if (queue == NULL || out_item == NULL)
        return H2_PAL_QUEUE_ERR_INVALID_ARG;
    while (queue->count == 0u && queue == env->drain_queue) {
        int16_t samples[4] = {0};
        const h2_audio_pcm_format_t format = {
            .sample_rate_hz = 16000u,
            .frame_samples_per_channel = 4u,
            .channels = 1u,
            .sample_format = H2_AUDIO_SAMPLE_S16LE,
        };
        h2_audio_frame_t frame =
            h2_audio_frame_for_buffer(samples, sizeof(samples), format);
        assert(h2_audio_mixer_read(env->mixer, &frame) == H2_AUDIO_OK);
        if (env->mixed_frames == 0u)
            memcpy(env->first_mixed, samples, sizeof(samples));
        ++env->mixed_frames;
    }
    if (queue->count == 0u)
        return queue->closed ? H2_PAL_QUEUE_ERR_CLOSED
                             : H2_PAL_QUEUE_ERR_TIMEOUT;
    memcpy(out_item, queue->items[queue->head], queue->item_size);
    queue->head = (queue->head + 1u) % queue->capacity;
    --queue->count;
    return H2_PAL_QUEUE_OK;
}

static int test_queue_reset(void *user, h2_pal_queue_t *opaque) {
    (void)user;
    test_queue_t *queue = (test_queue_t *)opaque;
    queue->head = 0u;
    queue->count = 0u;
    return H2_PAL_QUEUE_OK;
}

static int test_queue_close(void *user, h2_pal_queue_t *opaque) {
    (void)user;
    ((test_queue_t *)opaque)->closed = 1;
    return H2_PAL_QUEUE_OK;
}

int main(void) {
    test_env_t env = {0};
    static const h2_pal_queue_vtable_t queue_vtable = {
        .create = test_queue_create,
        .destroy = test_queue_destroy,
        .send = test_queue_send,
        .send_latest = test_queue_send_latest,
        .recv = test_queue_recv,
        .reset = test_queue_reset,
        .close = test_queue_close,
    };
    const h2_pal_queue_api_t queue_api = {
        .user = &env,
        .vtable = &queue_vtable,
    };
    static const h2_pal_sync_vtable_t sync_vtable = {
        .create_mutex = test_mutex_create,
        .destroy_mutex = test_mutex_destroy,
        .lock_mutex = test_mutex_lock,
        .unlock_mutex = test_mutex_unlock,
        .create_cond = test_cond_create,
        .destroy_cond = test_cond_destroy,
        .wait_cond = test_cond_wait,
        .broadcast_cond = test_cond_broadcast,
    };
    const h2_pal_sync_api_t sync_api = {
        .user = &env,
        .vtable = &sync_vtable,
    };
    const h2_audio_pcm_format_t format = {
        .sample_rate_hz = 16000u,
        .frame_samples_per_channel = 4u,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    const h2_audio_mixer_config_t config = {
        .format = format,
        .max_tracks = 1u,
        .track_queue_frames = 4u,
        .master_factor_milli = 1000u,
        .queue_api = &queue_api,
        .sync_api = &sync_api,
    };
    h2_audio_mixer_t mixer = {0};
    env.mixer = &mixer;
    assert(h2_audio_mixer_init(&mixer, &config) == H2_AUDIO_OK);

    const h2_audio_track_config_t track_config = {
        .name = "drain-test",
        .format = format,
        .volume_factor_milli = 1000u,
        .buffer_frames = 4u,
    };
    h2_pal_audio_track_t *track = NULL;
    assert(h2_audio_mixer_create_track(&mixer, NULL, &track_config, &track) ==
           H2_AUDIO_OK);

    int16_t samples[4] = {101, -202, 303, -404};
    h2_audio_frame_t frame =
        h2_audio_frame_for_buffer(samples, sizeof(samples), format);
    frame.bytes = sizeof(samples);
    assert(h2_pal_audio_track_write(track, &frame, 10u) == H2_AUDIO_OK);
    assert(h2_pal_audio_track_drain(track, 10u) == H2_AUDIO_OK);
    assert(env.mixed_frames == 2u);
    assert(memcmp(env.first_mixed, samples, sizeof(samples)) == 0);

    assert(h2_pal_audio_track_drain(track, 10u) == H2_AUDIO_OK);
    assert(env.mixed_frames == 3u);
    assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);
    assert(h2_pal_audio_track_close(track) == H2_AUDIO_OK);

    h2_pal_audio_track_t *replacement = NULL;
    assert(h2_audio_mixer_create_track(&mixer, NULL, &track_config,
                                       &replacement) == H2_AUDIO_OK);
    const int16_t replacement_samples[4] = {-11, 22, -33, 44};
    frame = h2_audio_frame_for_buffer((void *)replacement_samples,
                                      sizeof(replacement_samples), format);
    frame.bytes = sizeof(replacement_samples);
    assert(h2_pal_audio_track_write(replacement, &frame, 10u) == H2_AUDIO_OK);
    int16_t mixed[4] = {0};
    h2_audio_frame_t mixed_frame =
        h2_audio_frame_for_buffer(mixed, sizeof(mixed), format);
    assert(h2_audio_mixer_read(&mixer, &mixed_frame) == H2_AUDIO_OK);
    assert(memcmp(mixed, replacement_samples, sizeof(mixed)) == 0);
    assert(h2_pal_audio_track_close(replacement) == H2_AUDIO_OK);
    h2_audio_mixer_deinit(&mixer);
    puts("PASS h2_audio_mixer drain and track reuse");
    return 0;
}
