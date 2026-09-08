/*
 * The contract a target gets when it builds audio level measurement out
 * (H2_RUNTIME_AUDIO_LEVELS=0, as bk3633 does): the getter answers
 * UNSUPPORTED without losing its argument checking, the audio proxy stops
 * wrapping the backend playback track, and capture still works untouched.
 *
 * This test links //libs/runtime:runtime_no_audio_levels, so it exercises the
 * disabled build on the host instead of trusting a cross-compile. The fixture
 * is deliberately self-contained: it is the smallest set of fake PAL vtables
 * that gets a Runtime up, modelled on tests/test_runtime.c but not sharing
 * anything with it, so neither test constrains the other's internals.
 */

#include "h2_runtime_internal.h"

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_gpio_irq.h"
#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/hal/h2_pal_wifi.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_queue {
    size_t item_size;
    size_t item_count;
    size_t count;
    size_t head;
    size_t tail;
    uint8_t *items;
    const h2_pal_mem_api_t *mem;
    int closed;
};

struct h2_pal_task {
    h2_pal_task_entry_t entry;
    void *ctx;
};

struct h2_pal_mutex {
    const h2_pal_mem_api_t *mem;
};

struct h2_pal_cond {
    const h2_pal_mem_api_t *mem;
};

/* ---------------------------------------------------------------- fixture */

typedef struct env_allocator {
    size_t alloc_calls;
    size_t free_calls;
    size_t live_allocations;
} env_allocator_t;

typedef struct test_env {
    env_allocator_t allocator_state;
    h2_pal_mem_api_t mem;
    uint64_t now_ms;
    h2_pal_time_api_t time;
    h2_pal_queue_api_t queue;
    h2_pal_periph_api_t periph;
    h2_runtime_component_mapper_t component_mapper;
    h2_pal_task_api_t task;
    h2_pal_sync_api_t sync;
} test_env_t;

static void *env_alloc(void *user, size_t len) {
    env_allocator_t *state = (env_allocator_t *)user;
    void *ptr = calloc(1u, len);
    state->alloc_calls += 1u;
    if (ptr != NULL) {
        state->live_allocations += 1u;
    }
    return ptr;
}

static void env_free(void *user, void *ptr) {
    env_allocator_t *state = (env_allocator_t *)user;
    state->free_calls += 1u;
    assert(ptr != NULL);
    assert(state->live_allocations != 0u);
    state->live_allocations -= 1u;
    free(ptr);
}

static h2_pal_result_t env_now(void *user, uint64_t *out_ms) {
    *out_ms = ((test_env_t *)user)->now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t env_sleep(void *user, uint32_t ms) {
    ((test_env_t *)user)->now_ms += ms;
    return H2_PAL_OK;
}

static int env_queue_create(
    void *user, const h2_pal_queue_config_t *config, h2_pal_queue_t **out_queue) {
    (void)user;
    h2_pal_queue_t *queue =
        (h2_pal_queue_t *)h2_pal_mem_alloc(config->allocator, sizeof(*queue));
    if (queue == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    queue->items = (uint8_t *)h2_pal_mem_alloc(
        config->allocator, config->item_size * config->item_count);
    if (queue->items == NULL) {
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    queue->count = 0u;
    queue->head = 0u;
    queue->tail = 0u;
    queue->closed = 0;
    queue->mem = config->allocator;
    *out_queue = queue;
    return H2_PAL_OK;
}

static void env_queue_destroy(void *user, h2_pal_queue_t *queue) {
    (void)user;
    const h2_pal_mem_api_t *mem = queue->mem;
    h2_pal_mem_free(mem, queue->items);
    h2_pal_mem_free(mem, queue);
}

static int env_queue_send(
    void *user, h2_pal_queue_t *queue, const void *item, uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    if (queue->closed != 0) {
        return H2_PAL_ERR_CLOSED;
    }
    if (queue->count == queue->item_count) {
        return H2_PAL_ERR_FULL;
    }
    memcpy(queue->items + queue->tail * queue->item_size, item, queue->item_size);
    queue->tail = (queue->tail + 1u) % queue->item_count;
    queue->count += 1u;
    return H2_PAL_OK;
}

static int env_queue_recv(
    void *user, h2_pal_queue_t *queue, void *out_item, uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    if (queue->count == 0u) {
        return queue->closed != 0 ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_WOULD_BLOCK;
    }
    memcpy(out_item, queue->items + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1u) % queue->item_count;
    queue->count -= 1u;
    return H2_PAL_OK;
}

static int env_queue_reset(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    queue->count = 0u;
    queue->head = 0u;
    queue->tail = 0u;
    queue->closed = 0;
    return H2_PAL_OK;
}

static int env_queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    queue->closed = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t env_periph_list(
    void *user,
    h2_pal_periph_type_t type_filter,
    h2_pal_periph_cb_t cb,
    void *cb_user) {
    (void)user;
    (void)type_filter;
    (void)cb;
    (void)cb_user;
    return H2_PAL_OK;
}

static h2_pal_result_t env_periph_get(
    void *user, h2_pal_periph_id_t id, h2_pal_periph_info_t *out_info) {
    (void)user;
    (void)id;
    (void)out_info;
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t env_mapper_list(
    void *user,
    h2_runtime_component_t component_filter,
    h2_runtime_component_mapping_cb_t cb,
    void *cb_user) {
    (void)user;
    (void)component_filter;
    (void)cb_user;
    return cb == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_OK;
}

static int env_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    (void)user;
    (void)options;
    /* The Runtime tasks are never run here: this test drives the audio proxy
     * directly, so a handle that only records the entry point is enough. */
    h2_pal_task_t *handle = (h2_pal_task_t *)calloc(1u, sizeof(*handle));
    if (handle == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    handle->entry = entry;
    handle->ctx = ctx;
    *out_task = handle;
    return H2_PAL_OK;
}

static int env_task_join(void *user, h2_pal_task_t *handle) {
    (void)user;
    if (handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    free(handle);
    return H2_PAL_OK;
}

static h2_pal_result_t env_mutex_create(
    void *user, const h2_pal_mutex_config_t *config, h2_pal_mutex_t **out_mutex) {
    (void)user;
    h2_pal_mutex_t *mutex =
        (h2_pal_mutex_t *)h2_pal_mem_alloc(config->allocator, sizeof(*mutex));
    if (mutex == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    mutex->mem = config->allocator;
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t env_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    h2_pal_mem_free(mutex->mem, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t env_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    (void)mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t env_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    (void)mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t env_cond_create(
    void *user, const h2_pal_cond_config_t *config, h2_pal_cond_t **out_cond) {
    (void)user;
    h2_pal_cond_t *cond =
        (h2_pal_cond_t *)h2_pal_mem_alloc(config->allocator, sizeof(*cond));
    if (cond == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    cond->mem = config->allocator;
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t env_cond_destroy(void *user, h2_pal_cond_t *cond) {
    (void)user;
    h2_pal_mem_free(cond->mem, cond);
    return H2_PAL_OK;
}

static h2_pal_result_t env_cond_wait(
    void *user, h2_pal_cond_t *cond, h2_pal_mutex_t *mutex, uint32_t timeout_ms) {
    (void)user;
    (void)cond;
    (void)mutex;
    (void)timeout_ms;
    return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t env_cond_broadcast(void *user, h2_pal_cond_t *cond) {
    (void)user;
    (void)cond;
    return H2_PAL_OK;
}

static void env_init(test_env_t *env) {
    memset(env, 0, sizeof(*env));
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = env_alloc,
        .free = env_free,
    };
    env->mem = (h2_pal_mem_api_t){
        .user = &env->allocator_state,
        .vtable = &mem_vtable,
    };
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = env_now,
        .sleep_ms = env_sleep,
    };
    env->time = (h2_pal_time_api_t){.user = env, .vtable = &time_vtable};
    static const h2_pal_queue_vtable_t queue_vtable = {
        .create = env_queue_create,
        .destroy = env_queue_destroy,
        .send = env_queue_send,
        .recv = env_queue_recv,
        .reset = env_queue_reset,
        .close = env_queue_close,
    };
    env->queue = (h2_pal_queue_api_t){.user = env, .vtable = &queue_vtable};
    static const h2_pal_periph_vtable_t periph_vtable = {
        .list = env_periph_list,
        .get = env_periph_get,
    };
    env->periph = (h2_pal_periph_api_t){.user = env, .vtable = &periph_vtable};
    static const h2_runtime_component_mapper_vtable_t mapper_vtable = {
        .list = env_mapper_list,
    };
    env->component_mapper = (h2_runtime_component_mapper_t){
        .user = env,
        .vtable = &mapper_vtable,
    };
    static const h2_pal_task_vtable_t task_vtable = {
        .start = env_task_start,
        .join = env_task_join,
    };
    env->task = (h2_pal_task_api_t){.user = env, .vtable = &task_vtable};
    static const h2_pal_sync_vtable_t sync_vtable = {
        .create_mutex = env_mutex_create,
        .destroy_mutex = env_mutex_destroy,
        .lock_mutex = env_mutex_lock,
        .unlock_mutex = env_mutex_unlock,
        .create_cond = env_cond_create,
        .destroy_cond = env_cond_destroy,
        .wait_cond = env_cond_wait,
        .broadcast_cond = env_cond_broadcast,
    };
    env->sync = (h2_pal_sync_api_t){.user = env, .vtable = &sync_vtable};
}

static h2_runtime_config_t env_runtime_config(test_env_t *env) {
    return (h2_runtime_config_t){
        .board = "test-board",
        .target = "host",
        .chip = "host",
        .firmware_info = h2_pal_unsupported_firmware_info_api(),
        .log = h2_pal_unsupported_log_api(),
        .time = &env->time,
        .timer = h2_pal_unsupported_timer_api(),
        .queue = &env->queue,
        .fs = h2_pal_unsupported_fs_api(),
        .disk = h2_pal_unsupported_disk_api(),
        .pref = h2_pal_unsupported_pref_api(),
        .crypto = h2_pal_unsupported_crypto_api(),
        .http = h2_pal_unsupported_http_api(),
        .net = h2_pal_unsupported_net_api(),
        .netif = h2_pal_unsupported_netif_api(),
        .mqtt = h2_pal_unsupported_mqtt_api(),
        .webrtc = h2_pal_unsupported_webrtc_api(),
        .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
        .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
        .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
        .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
        .ble_host = h2_pal_unsupported_ble_host_api(),
        .modem = h2_pal_unsupported_modem_api(),
        .power = h2_pal_unsupported_power_api(),
        .display = h2_pal_unsupported_display_api(),
        .audio = h2_pal_unsupported_audio_api(),
        .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
        .periph = &env->periph,
        .button = h2_pal_unsupported_button_api(),
        .touch = h2_pal_unsupported_touch_api(),
        .buzzer = h2_pal_unsupported_buzzer_api(),
        .nfc = h2_pal_unsupported_nfc_api(),
        .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
        .imu = h2_pal_unsupported_imu_api(),
        .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
        .led = h2_pal_unsupported_led_api(),
        .switch_api = h2_pal_unsupported_switch_api(),
        .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
        .input = h2_pal_unsupported_input_api(),
        .task = &env->task,
        .sync = &env->sync,
        .mem = &env->mem,
        .system_event = h2_pal_unsupported_system_event_api(),
        .video_decoder = h2_pal_unsupported_video_decoder_api(),
        .component_mapper = &env->component_mapper,
        .event_queue_capacity = 4u,
    };
}

/* ------------------------------------------------------------ audio fake */

typedef struct audio_fixture {
    int16_t mic_samples[4];
    size_t mic_bytes;
    h2_pal_audio_track_t backend_track;
    unsigned int writes;
    unsigned int closes;
    unsigned int mic_reads;
    unsigned int creates;
} audio_fixture_t;

static int fake_mic_read(void *user, h2_audio_frame_t *frame, uint32_t timeout_ms) {
    audio_fixture_t *f = user;
    (void)timeout_ms;
    f->mic_reads += 1u;
    memcpy(frame->data, f->mic_samples, f->mic_bytes);
    frame->bytes = f->mic_bytes;
    frame->sample_format = H2_AUDIO_SAMPLE_S16LE;
    frame->channels = 1u;
    frame->samples_per_channel = (uint16_t)(f->mic_bytes / sizeof(int16_t));
    return H2_PAL_OK;
}

static int fake_track_write(
    h2_pal_audio_track_t *track, const h2_audio_frame_t *frame, uint32_t timeout_ms) {
    audio_fixture_t *f = track->user;
    (void)frame;
    (void)timeout_ms;
    f->writes += 1u;
    return H2_PAL_OK;
}

static int fake_track_close(h2_pal_audio_track_t *track) {
    ((audio_fixture_t *)track->user)->closes += 1u;
    return H2_PAL_OK;
}

static int fake_create_track(
    void *user, const h2_audio_track_config_t *config, h2_pal_audio_track_t **out) {
    audio_fixture_t *f = user;
    (void)config;
    f->creates += 1u;
    f->backend_track = (h2_pal_audio_track_t){
        .user = f,
        .write = fake_track_write,
        .close = fake_track_close,
    };
    *out = &f->backend_track;
    return H2_PAL_OK;
}

static const h2_pal_audio_vtable_t fake_audio_vtable = {
    .mic_read = fake_mic_read,
    .create_track = fake_create_track,
};

/* -------------------------------------------------------------- the tests */

/* The getter still validates its arguments before reporting that the feature
 * is absent: a caller that passes NULL learns it passed NULL, so INVALID_ARG
 * takes precedence over UNSUPPORTED. */
static void test_get_levels_checks_arguments_before_reporting_unsupported(void) {
    test_env_t env;
    env_init(&env);
    audio_fixture_t f = {
        .mic_samples = {0, -16384, 4096, 0},
        .mic_bytes = 4u * sizeof(int16_t),
    };
    const h2_pal_audio_api_t audio = {&f, &fake_audio_vtable};
    h2_runtime_config_t config = env_runtime_config(&env);
    config.audio = &audio;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);

    h2_runtime_audio_levels_t levels;
    assert(h2_runtime_audio_get_levels(NULL, &levels) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_runtime_audio_get_levels(runtime, NULL) == H2_PAL_ERR_INVALID_ARG);

    /* A ready runtime with both arguments valid is the case that reports the
     * feature is built out. */
    assert(h2_runtime_audio_get_levels(runtime, &levels) == H2_PAL_ERR_UNSUPPORTED);

    h2_runtime_deinit(runtime);
    assert(env.allocator_state.live_allocations == 0u);
}

/* Without measurement there is nothing to observe playback for, so the proxy
 * hands out the backend's own track: no wrapper object, no extra allocation,
 * and every operation reaching the backend exactly once. */
static void test_create_track_forwards_the_backend_track(void) {
    test_env_t env;
    env_init(&env);
    audio_fixture_t f = {
        .mic_samples = {0, -16384, 4096, 0},
        .mic_bytes = 4u * sizeof(int16_t),
    };
    const h2_pal_audio_api_t audio = {&f, &fake_audio_vtable};
    h2_runtime_config_t config = env_runtime_config(&env);
    config.audio = &audio;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);

    size_t allocs_before = env.allocator_state.alloc_calls;
    size_t frees_before = env.allocator_state.free_calls;

    h2_pal_audio_track_t *track = NULL;
    h2_audio_track_config_t track_config = {.name = "disabled"};
    assert(h2_pal_audio_create_track(runtime->audio, &track_config, &track) == H2_PAL_OK);
    assert(f.creates == 1u);
    assert(track == &f.backend_track);
    assert(env.allocator_state.alloc_calls == allocs_before);

    int16_t played[2] = {8192, -2048};
    h2_audio_frame_t out_frame = {
        .data = played,
        .capacity = sizeof(played),
        .bytes = sizeof(played),
        .channels = 1u,
        .samples_per_channel = 2u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE};
    assert(h2_pal_audio_track_write(track, &out_frame, 0u) == H2_PAL_OK);
    assert(f.writes == 1u);

    assert(h2_pal_audio_track_close(track) == H2_PAL_OK);
    assert(f.closes == 1u);
    /* Closing the backend track directly frees nothing of the Runtime's. */
    assert(env.allocator_state.free_calls == frees_before);
    assert(env.allocator_state.alloc_calls == allocs_before);

    h2_runtime_deinit(runtime);
    assert(env.allocator_state.live_allocations == 0u);
}

/* Capture is untouched: the proxy forwards the read and returns the backend
 * frame as it came, it just does not measure it. */
static void test_mic_read_still_forwards_to_the_backend(void) {
    test_env_t env;
    env_init(&env);
    audio_fixture_t f = {
        .mic_samples = {0, -16384, 4096, 0},
        .mic_bytes = 4u * sizeof(int16_t),
    };
    const h2_pal_audio_api_t audio = {&f, &fake_audio_vtable};
    h2_runtime_config_t config = env_runtime_config(&env);
    config.audio = &audio;
    env.now_ms = 1200u;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);

    int16_t buffer[8];
    h2_audio_frame_t frame = {
        .data = buffer,
        .capacity = sizeof(buffer),
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE};
    assert(h2_pal_audio_mic_read(runtime->audio, &frame, 0u) == H2_PAL_OK);
    assert(f.mic_reads == 1u);
    assert(frame.bytes == f.mic_bytes);
    assert(frame.samples_per_channel == 4u);
    assert(frame.sample_format == H2_AUDIO_SAMPLE_S16LE);
    assert(memcmp(buffer, f.mic_samples, f.mic_bytes) == 0);

    /* Reading a frame does not make levels available. */
    h2_runtime_audio_levels_t levels;
    assert(h2_runtime_audio_get_levels(runtime, &levels) == H2_PAL_ERR_UNSUPPORTED);

    h2_runtime_deinit(runtime);
    assert(env.allocator_state.live_allocations == 0u);
}

int main(void) {
    test_get_levels_checks_arguments_before_reporting_unsupported();
    test_create_track_forwards_the_backend_track();
    test_mic_read_still_forwards_to_the_backend();
    return 0;
}
