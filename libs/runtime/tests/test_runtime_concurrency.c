#define _POSIX_C_SOURCE 200809L

#include "h2_runtime_internal.h"
#include "h2_runtime_test.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct h2_pal_mutex {
    pthread_mutex_t native;
    const h2_pal_mem_api_t *mem;
};

struct h2_pal_cond {
    pthread_cond_t native;
    const h2_pal_mem_api_t *mem;
};

struct h2_pal_task {
    h2_pal_task_entry_t entry;
    void *ctx;
};

struct h2_pal_queue {
    size_t item_size;
    size_t item_count;
    size_t count;
    size_t head;
    size_t tail;
    uint8_t *items;
    const h2_pal_mem_api_t *mem;
};

typedef struct concurrency_allocator {
    atomic_size_t alloc_calls;
    atomic_size_t free_calls;
} concurrency_allocator_t;

typedef struct concurrency_time {
    atomic_uint_fast64_t now_ms;
} concurrency_time_t;

typedef struct concurrency_sync {
    atomic_int fail_signal_once;
    atomic_size_t signal_calls;
    atomic_size_t broadcast_calls;
} concurrency_sync_t;

typedef struct concurrency_periphs {
    h2_pal_periph_info_t infos[H2_RUNTIME_DEFAULT_INPUT_SOURCE_CAPACITY];
    size_t count;
} concurrency_periphs_t;

typedef struct concurrency_button {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int block_single_read;
    int single_read_entered;
    h2_pal_button_state_t single_state;
    h2_pal_periph_id_t radio_pressed_id;
    h2_pal_result_t group_result;
    size_t single_reads;
    size_t group_reads;
} concurrency_button_t;

typedef struct concurrency_env {
    concurrency_allocator_t allocator_state;
    h2_pal_mem_api_t mem;
    concurrency_time_t time_state;
    h2_pal_time_api_t time;
    concurrency_sync_t sync_state;
    h2_pal_sync_api_t sync;
    h2_pal_task_api_t task;
    h2_pal_queue_api_t queue;
    concurrency_periphs_t periphs;
    h2_pal_periph_api_t periph;
    h2_runtime_component_mapper_t mapper;
    concurrency_button_t button_state;
    h2_pal_button_api_t button;
} concurrency_env_t;

typedef struct runtime_call {
    h2_runtime_t *runtime;
    h2_pal_result_t result;
} runtime_call_t;

static void *concurrency_alloc(void *user, size_t len) {
    concurrency_allocator_t *allocator =
        (concurrency_allocator_t *)user;
    atomic_fetch_add(&allocator->alloc_calls, 1u);
    return calloc(1u, len);
}

static void concurrency_free(void *user, void *ptr) {
    concurrency_allocator_t *allocator =
        (concurrency_allocator_t *)user;
    atomic_fetch_add(&allocator->free_calls, 1u);
    free(ptr);
}

static h2_pal_result_t concurrency_now(
    void *user,
    uint64_t *out_ms) {
    concurrency_time_t *time = (concurrency_time_t *)user;
    *out_ms = atomic_load(&time->now_ms);
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_mutex_create(
    void *user,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    (void)user;
    h2_pal_mutex_t *mutex =
        (h2_pal_mutex_t *)h2_pal_mem_alloc(
            config->allocator,
            sizeof(*mutex));
    if (mutex == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (pthread_mutex_init(&mutex->native, NULL) != 0) {
        h2_pal_mem_free(config->allocator, mutex);
        return H2_PAL_ERR_IO;
    }
    mutex->mem = config->allocator;
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_mutex_destroy(
    void *user,
    h2_pal_mutex_t *mutex) {
    (void)user;
    if (pthread_mutex_destroy(&mutex->native) != 0) {
        return H2_PAL_ERR_IO;
    }
    h2_pal_mem_free(mutex->mem, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_mutex_lock(
    void *user,
    h2_pal_mutex_t *mutex) {
    (void)user;
    return pthread_mutex_lock(&mutex->native) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t concurrency_mutex_unlock(
    void *user,
    h2_pal_mutex_t *mutex) {
    (void)user;
    return pthread_mutex_unlock(&mutex->native) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t concurrency_cond_create(
    void *user,
    const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
    (void)user;
    h2_pal_cond_t *cond =
        (h2_pal_cond_t *)h2_pal_mem_alloc(
            config->allocator,
            sizeof(*cond));
    if (cond == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (pthread_cond_init(&cond->native, NULL) != 0) {
        h2_pal_mem_free(config->allocator, cond);
        return H2_PAL_ERR_IO;
    }
    cond->mem = config->allocator;
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_cond_destroy(
    void *user,
    h2_pal_cond_t *cond) {
    (void)user;
    if (pthread_cond_destroy(&cond->native) != 0) {
        return H2_PAL_ERR_IO;
    }
    h2_pal_mem_free(cond->mem, cond);
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_cond_wait(
    void *user,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    (void)user;
    if (timeout_ms == H2_PAL_SYNC_WAIT_FOREVER) {
        return pthread_cond_wait(&cond->native, &mutex->native) == 0
                   ? H2_PAL_OK
                   : H2_PAL_ERR_IO;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return H2_PAL_ERR_IO;
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec +=
        (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    int result = pthread_cond_timedwait(
        &cond->native,
        &mutex->native,
        &deadline);
    return result == 0
               ? H2_PAL_OK
               : (result == ETIMEDOUT
                      ? H2_PAL_ERR_TIMEOUT
                      : H2_PAL_ERR_IO);
}

static h2_pal_result_t concurrency_cond_signal(
    void *user,
    h2_pal_cond_t *cond) {
    concurrency_sync_t *sync = (concurrency_sync_t *)user;
    atomic_fetch_add(&sync->signal_calls, 1u);
    if (atomic_exchange(&sync->fail_signal_once, 0) != 0) {
        return H2_PAL_ERR_IO;
    }
    return pthread_cond_signal(&cond->native) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t concurrency_cond_broadcast(
    void *user,
    h2_pal_cond_t *cond) {
    concurrency_sync_t *sync = (concurrency_sync_t *)user;
    atomic_fetch_add(&sync->broadcast_calls, 1u);
    return pthread_cond_broadcast(&cond->native) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t concurrency_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    (void)user;
    (void)options;
    h2_pal_task_t *task =
        (h2_pal_task_t *)calloc(1u, sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    task->entry = entry;
    task->ctx = ctx;
    *out_task = task;
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_task_join(
    void *user,
    h2_pal_task_t *task) {
    (void)user;
    free(task);
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_queue_create(
    void *user,
    const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
    (void)user;
    h2_pal_queue_t *queue =
        (h2_pal_queue_t *)h2_pal_mem_alloc(
            config->allocator,
            sizeof(*queue));
    if (queue == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    queue->items = (uint8_t *)h2_pal_mem_alloc(
        config->allocator,
        config->item_size * config->item_count);
    if (queue->items == NULL) {
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    queue->mem = config->allocator;
    *out_queue = queue;
    return H2_PAL_OK;
}

static void concurrency_queue_destroy(
    void *user,
    h2_pal_queue_t *queue) {
    (void)user;
    const h2_pal_mem_api_t *mem = queue->mem;
    h2_pal_mem_free(mem, queue->items);
    h2_pal_mem_free(mem, queue);
}

static h2_pal_result_t concurrency_queue_send(
    void *user,
    h2_pal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    if (queue->count == queue->item_count) {
        return H2_PAL_ERR_FULL;
    }
    memcpy(
        queue->items + queue->tail * queue->item_size,
        item,
        queue->item_size);
    queue->tail = (queue->tail + 1u) % queue->item_count;
    queue->count += 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_queue_recv(
    void *user,
    h2_pal_queue_t *queue,
    void *out_item,
    uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    if (queue->count == 0u) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    memcpy(
        out_item,
        queue->items + queue->head * queue->item_size,
        queue->item_size);
    queue->head = (queue->head + 1u) % queue->item_count;
    queue->count -= 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_periph_get(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_periph_info_t *out_info) {
    concurrency_periphs_t *periphs =
        (concurrency_periphs_t *)user;
    for (size_t i = 0u; i < periphs->count; ++i) {
        if (periphs->infos[i].id == id) {
            *out_info = periphs->infos[i];
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t concurrency_periph_list(
    void *user,
    h2_pal_periph_type_t filter,
    h2_pal_periph_cb_t cb,
    void *cb_user) {
    concurrency_periphs_t *periphs =
        (concurrency_periphs_t *)user;
    for (size_t i = 0u; i < periphs->count; ++i) {
        if (filter != H2_PAL_PERIPH_TYPE_ANY &&
            filter != periphs->infos[i].type) {
            continue;
        }
        h2_pal_result_t rc = cb(cb_user, &periphs->infos[i]);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_mapper_list(
    void *user,
    h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t cb,
    void *cb_user) {
    concurrency_periphs_t *periphs =
        (concurrency_periphs_t *)user;
    if (filter != H2_RUNTIME_COMPONENT_BUTTON) {
        return H2_PAL_OK;
    }
    for (size_t i = 0u; i < periphs->count; ++i) {
        const h2_runtime_component_mapping_entry_t entry = {
            .component_id =
                (h2_runtime_component_id_t)(i + 1u),
            .periph_id = periphs->infos[i].id,
        };
        h2_pal_result_t rc = cb(cb_user, &entry);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_read_single(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t *out_reading) {
    concurrency_button_t *button = (concurrency_button_t *)user;
    assert(pthread_mutex_lock(&button->mutex) == 0);
    button->single_reads += 1u;
    button->single_read_entered = 1;
    assert(pthread_cond_broadcast(&button->cond) == 0);
    while (button->block_single_read != 0) {
        assert(pthread_cond_wait(&button->cond, &button->mutex) == 0);
    }
    *out_reading = (h2_pal_single_button_reading_t){
        .id = id,
        .state = button->single_state,
    };
    assert(pthread_mutex_unlock(&button->mutex) == 0);
    return H2_PAL_OK;
}

static h2_pal_result_t concurrency_read_radio_group(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t *out_reading) {
    concurrency_button_t *button = (concurrency_button_t *)user;
    button->group_reads += 1u;
    if (button->group_result != H2_PAL_OK) {
        return button->group_result;
    }
    *out_reading = (h2_pal_radio_button_group_reading_t){
        .id = id,
        .pressed_button_id = button->radio_pressed_id,
    };
    return H2_PAL_OK;
}

static void concurrency_env_init(concurrency_env_t *env) {
    memset(env, 0, sizeof(*env));
    atomic_init(&env->allocator_state.alloc_calls, 0u);
    atomic_init(&env->allocator_state.free_calls, 0u);
    atomic_init(&env->time_state.now_ms, 0u);
    atomic_init(&env->sync_state.fail_signal_once, 0);
    atomic_init(&env->sync_state.signal_calls, 0u);
    atomic_init(&env->sync_state.broadcast_calls, 0u);
    assert(pthread_mutex_init(&env->button_state.mutex, NULL) == 0);
    assert(pthread_cond_init(&env->button_state.cond, NULL) == 0);

    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = concurrency_alloc,
        .free = concurrency_free,
    };
    env->mem = (h2_pal_mem_api_t){
        .user = &env->allocator_state,
        .vtable = &mem_vtable,
    };
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = concurrency_now,
    };
    env->time = (h2_pal_time_api_t){
        .user = &env->time_state,
        .vtable = &time_vtable,
    };
    static const h2_pal_sync_vtable_t sync_vtable = {
        .create_mutex = concurrency_mutex_create,
        .destroy_mutex = concurrency_mutex_destroy,
        .lock_mutex = concurrency_mutex_lock,
        .unlock_mutex = concurrency_mutex_unlock,
        .create_cond = concurrency_cond_create,
        .destroy_cond = concurrency_cond_destroy,
        .wait_cond = concurrency_cond_wait,
        .signal_cond = concurrency_cond_signal,
        .broadcast_cond = concurrency_cond_broadcast,
    };
    env->sync = (h2_pal_sync_api_t){
        .user = &env->sync_state,
        .vtable = &sync_vtable,
    };
    static const h2_pal_task_vtable_t task_vtable = {
        .start = concurrency_task_start,
        .join = concurrency_task_join,
    };
    env->task =
        (h2_pal_task_api_t){ .user = NULL, .vtable = &task_vtable };
    static const h2_pal_queue_vtable_t queue_vtable = {
        .create = concurrency_queue_create,
        .destroy = concurrency_queue_destroy,
        .send = concurrency_queue_send,
        .recv = concurrency_queue_recv,
    };
    env->queue =
        (h2_pal_queue_api_t){ .user = NULL, .vtable = &queue_vtable };
    static const h2_pal_periph_vtable_t periph_vtable = {
        .list = concurrency_periph_list,
        .get = concurrency_periph_get,
    };
    env->periph = (h2_pal_periph_api_t){
        .user = &env->periphs,
        .vtable = &periph_vtable,
    };
    static const h2_runtime_component_mapper_vtable_t mapper_vtable = {
        .list = concurrency_mapper_list,
    };
    env->mapper = (h2_runtime_component_mapper_t){
        .user = &env->periphs,
        .vtable = &mapper_vtable,
    };
    static const h2_pal_button_vtable_t button_vtable = {
        .read_single_button = concurrency_read_single,
        .read_radio_button_group = concurrency_read_radio_group,
    };
    env->button = (h2_pal_button_api_t){
        .user = &env->button_state,
        .vtable = &button_vtable,
    };
}

static void concurrency_env_deinit(concurrency_env_t *env) {
    assert(pthread_cond_destroy(&env->button_state.cond) == 0);
    assert(pthread_mutex_destroy(&env->button_state.mutex) == 0);
}

static h2_runtime_config_t concurrency_runtime_config(
    concurrency_env_t *env) {
    return (h2_runtime_config_t){
        .board = "concurrency-test",
        .target = "host",
        .chip = "host",
        .firmware_info = h2_pal_unsupported_firmware_info_api(),
        .mem = &env->mem,
        .log = h2_pal_unsupported_log_api(),
        .time = &env->time,
        .timer = h2_pal_unsupported_timer_api(),
        .task = &env->task,
        .queue = &env->queue,
        .sync = &env->sync,
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
        .video_decoder = h2_pal_unsupported_video_decoder_api(),
        .audio = h2_pal_unsupported_audio_api(),
        .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
        .periph = &env->periph,
        .button = &env->button,
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
        .system_event = h2_pal_unsupported_system_event_api(),
        .component_mapper = &env->mapper,
        .event_queue_capacity = 64u,
    };
}

static h2_runtime_t *concurrency_runtime_create(
    concurrency_env_t *env) {
    h2_runtime_config_t config = concurrency_runtime_config(env);
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime != NULL);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    return runtime;
}

static void add_single_button(concurrency_env_t *env) {
    env->periphs.infos[0] = (h2_pal_periph_info_t){
        .id = 10u,
        .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
    };
    env->periphs.count = 1u;
}

static void add_poll_and_push_buttons(concurrency_env_t *env) {
    static const h2_pal_periph_single_button_payload_t push_payload = {
        .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
    };
    env->periphs.infos[0] = (h2_pal_periph_info_t){
        .id = 10u,
        .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
    };
    env->periphs.infos[1] = (h2_pal_periph_info_t){
        .id = 20u,
        .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
        .payload = &push_payload,
        .payload_size = sizeof(push_payload),
    };
    env->periphs.count = 2u;
}

static void add_radio_buttons(concurrency_env_t *env) {
    static const h2_pal_periph_radio_button_payload_t payload = {
        .group_id = 100u,
    };
    for (size_t i = 0u; i < H2_RUNTIME_DEFAULT_INPUT_SOURCE_CAPACITY; ++i) {
        env->periphs.infos[i] = (h2_pal_periph_info_t){
            .id = (h2_pal_periph_id_t)(i + 1u),
            .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
            .payload = &payload,
            .payload_size = sizeof(payload),
        };
    }
    env->periphs.count = H2_RUNTIME_DEFAULT_INPUT_SOURCE_CAPACITY;
}

static void *poll_thread(void *user) {
    runtime_call_t *call = (runtime_call_t *)user;
    call->result = h2_runtime_input_poll_once(call->runtime);
    return NULL;
}

static void test_slow_pal_does_not_block_snapshot_read(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_single_button(&env);
    h2_runtime_t *runtime = concurrency_runtime_create(&env);

    h2_runtime_button_state_t state;
    memset(&state, 0x7f, sizeof(state));
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(!state.pressed);

    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(!state.pressed);

    assert(pthread_mutex_lock(&env.button_state.mutex) == 0);
    env.button_state.block_single_read = 1;
    env.button_state.single_read_entered = 0;
    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    assert(pthread_mutex_unlock(&env.button_state.mutex) == 0);
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);

    runtime_call_t poll = { .runtime = runtime };
    pthread_t poll_handle;
    assert(pthread_create(
               &poll_handle,
               NULL,
               poll_thread,
               &poll) == 0);
    assert(pthread_mutex_lock(&env.button_state.mutex) == 0);
    while (env.button_state.single_read_entered == 0) {
        assert(pthread_cond_wait(
                   &env.button_state.cond,
                   &env.button_state.mutex) == 0);
    }
    assert(pthread_mutex_unlock(&env.button_state.mutex) == 0);

    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(!state.pressed);

    assert(pthread_mutex_lock(&env.button_state.mutex) == 0);
    env.button_state.block_single_read = 0;
    assert(pthread_cond_broadcast(&env.button_state.cond) == 0);
    assert(pthread_mutex_unlock(&env.button_state.mutex) == 0);
    assert(pthread_join(poll_handle, NULL) == 0);
    assert(poll.result == H2_PAL_OK);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = payload,
        .payload_capacity = sizeof(payload),
    };
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

static void test_push_edge_does_not_write_while_poller_is_blocked(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_poll_and_push_buttons(&env);
    h2_runtime_t *runtime = concurrency_runtime_create(&env);

    assert(pthread_mutex_lock(&env.button_state.mutex) == 0);
    env.button_state.block_single_read = 1;
    env.button_state.single_read_entered = 0;
    assert(pthread_mutex_unlock(&env.button_state.mutex) == 0);
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);

    runtime_call_t poll = {.runtime = runtime};
    pthread_t poll_handle;
    assert(pthread_create(
               &poll_handle, NULL, poll_thread, &poll) == 0);
    assert(pthread_mutex_lock(&env.button_state.mutex) == 0);
    while (env.button_state.single_read_entered == 0) {
        assert(pthread_cond_wait(
                   &env.button_state.cond,
                   &env.button_state.mutex) == 0);
    }
    assert(pthread_mutex_unlock(&env.button_state.mutex) == 0);

    assert(h2_runtime_button_push_edge(
               runtime, 20u, H2_RUNTIME_BUTTON_EDGE_DOWN) == H2_PAL_OK);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = payload,
        .payload_capacity = sizeof(payload),
    };
    assert(h2_runtime_poll_event(runtime, &event) ==
           H2_PAL_ERR_WOULD_BLOCK);

    assert(pthread_mutex_lock(&env.button_state.mutex) == 0);
    env.button_state.block_single_read = 0;
    assert(pthread_cond_broadcast(&env.button_state.cond) == 0);
    assert(pthread_mutex_unlock(&env.button_state.mutex) == 0);
    assert(pthread_join(poll_handle, NULL) == 0);
    assert(poll.result == H2_PAL_OK);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    assert(event.component_id == 2u);

    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

static void test_pinned_reader_does_not_block_publication(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_single_button(&env);
    h2_runtime_t *runtime = concurrency_runtime_create(&env);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    const h2_runtime_state_bank_t *bank = NULL;
    uint8_t slot_index = UINT8_MAX;
    assert(h2_runtime_state_read_begin(
               runtime, &bank, &slot_index) ==
           H2_PAL_OK);
    assert(bank != NULL);
    const unsigned int initial_index = atomic_load_explicit(
        &runtime->private_state->state_publication.active_index,
        memory_order_acquire);
    assert(slot_index == initial_index);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(atomic_load_explicit(
               &runtime->private_state->state_publication.active_index,
               memory_order_acquire) != initial_index);
    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed);
    assert(h2_runtime_state_read_end(runtime, slot_index) ==
           H2_PAL_OK);

    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

static void test_restart_keeps_publication_and_reader_pins_valid(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_single_button(&env);
    h2_runtime_t *runtime = concurrency_runtime_create(&env);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;

    /*
     * The publication belongs to the Runtime, not to the poller, so a reader
     * pinned across a stop keeps a valid slot and releases it normally. State
     * stays readable the whole time the poller is off.
     */
    const h2_runtime_state_bank_t *bank = NULL;
    uint8_t slot_index = UINT8_MAX;
    assert(h2_runtime_state_read_begin(runtime, &bank, &slot_index) ==
           H2_PAL_OK);
    assert(h2_runtime_input_stop(runtime) == H2_PAL_OK);
    assert(h2_runtime_state_publication_ready(runtime));
    assert(h2_runtime_state_read_end(runtime, slot_index) == H2_PAL_OK);
    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);

    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    for (size_t index = 0u; index < H2_RUNTIME_STATE_SLOT_COUNT; ++index) {
        assert(atomic_load_explicit(
                   &publication->reader_count[index],
                   memory_order_relaxed) == 0u);
    }

    /* The rebuilt publication still rotates slots for a concurrent reader. */
    assert(h2_runtime_state_read_begin(runtime, &bank, &slot_index) ==
           H2_PAL_OK);
    assert(bank != NULL);
    const unsigned int pinned_index = atomic_load_explicit(
        &publication->active_index, memory_order_acquire);
    assert(slot_index == pinned_index);
    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS * 4u);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(atomic_load_explicit(
               &publication->active_index, memory_order_acquire) !=
           pinned_index);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed);
    assert(h2_runtime_state_read_end(runtime, slot_index) == H2_PAL_OK);

    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

static void test_publication_defers_when_all_retired_slots_are_pinned(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_single_button(&env);
    h2_runtime_t *runtime = concurrency_runtime_create(&env);
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;

    const h2_runtime_state_bank_t *bank = NULL;
    uint8_t slots[H2_RUNTIME_STATE_SLOT_COUNT] = {
        UINT8_MAX,
        UINT8_MAX,
        UINT8_MAX,
    };
    assert(h2_runtime_state_read_begin(
               runtime, &bank, &slots[0]) == H2_PAL_OK);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_state_read_begin(
               runtime, &bank, &slots[1]) == H2_PAL_OK);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS * 2u);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_state_read_begin(
               runtime, &bank, &slots[2]) == H2_PAL_OK);

    const unsigned int active_before = atomic_load_explicit(
        &publication->active_index, memory_order_acquire);
    const uint64_t deferred_before = publication->deferred_count;
    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS * 3u);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    /* The event pre-publish and the end-of-poll publish both defer. */
    assert(publication->deferred_count == deferred_before + 2u);
    assert(atomic_load_explicit(
               &publication->active_index, memory_order_acquire) ==
           active_before);
    assert(runtime->private_state->state_dirty != 0);

    assert(h2_runtime_state_read_end(runtime, slots[0]) ==
           H2_PAL_OK);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(atomic_load_explicit(
               &publication->active_index, memory_order_acquire) !=
           active_before);
    assert(runtime->private_state->state_dirty == 0);
    assert(h2_runtime_state_read_end(runtime, slots[1]) ==
           H2_PAL_OK);
    assert(h2_runtime_state_read_end(runtime, slots[2]) ==
           H2_PAL_OK);

    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

static void test_publication_counts_and_event_ceiling(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_single_button(&env);
    h2_runtime_t *runtime = concurrency_runtime_create(&env);
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;
    uint64_t copies = publication->copy_count;
    uint64_t switches = publication->switch_count;
    env.button_state.group_reads = 0u;
    unsigned int initial_bank = atomic_load_explicit(
        &publication->active_index, memory_order_acquire);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(publication->copy_count == copies);
    assert(publication->switch_count == switches);
    assert(atomic_load_explicit(
               &publication->active_index, memory_order_acquire) ==
           initial_bank);
    size_t allocations =
        atomic_load(&env.allocator_state.alloc_calls);
    unsigned int first_poll_bank = atomic_load_explicit(
        &publication->active_index, memory_order_acquire);
    copies = publication->copy_count;
    switches = publication->switch_count;

    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(publication->copy_count == copies);
    assert(publication->switch_count == switches);
    assert(atomic_load_explicit(
               &publication->active_index, memory_order_acquire) ==
           first_poll_bank);
    copies = publication->copy_count;
    switches = publication->switch_count;

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(publication->copy_count == copies + 1u);
    assert(publication->switch_count == switches + 1u);
    assert(atomic_load_explicit(
               &publication->active_index, memory_order_acquire) !=
           first_poll_bank);
    copies = publication->copy_count;
    switches = publication->switch_count;

    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS * 2u);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(publication->copy_count == copies + 1u);
    assert(publication->switch_count == switches + 1u);
    assert(atomic_load_explicit(
               &publication->active_index, memory_order_acquire) !=
           first_poll_bank);
    assert(atomic_load(&env.allocator_state.alloc_calls) == allocations);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = payload,
        .payload_capacity = sizeof(payload),
    };
    h2_runtime_sequence_t last_sequence = 0u;
    size_t event_count = 0u;
    while (h2_runtime_poll_event(runtime, &event) == H2_PAL_OK) {
        last_sequence = event.sequence;
        event_count += 1u;
    }
    assert(event_count == 4u);
    const unsigned int active_index = atomic_load_explicit(
        &publication->active_index, memory_order_acquire);
    const h2_runtime_state_bank_t *active =
        &publication->banks[active_index];
    assert(active->event_sequence_ceiling >= last_sequence);

    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

static void test_radio_error_batch_uses_one_switch(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_radio_buttons(&env);
    env.button_state.group_result = H2_PAL_ERR_IO;
    h2_runtime_t *runtime = concurrency_runtime_create(&env);
    uint8_t initial_payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t initial_event = {
        .payload = initial_payload,
        .payload_capacity = sizeof(initial_payload),
    };
    while (h2_runtime_poll_event(runtime, &initial_event) == H2_PAL_OK) {
    }
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;
    uint64_t copies = publication->copy_count;
    uint64_t switches = publication->switch_count;
    env.button_state.group_reads = 0u;

    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(env.button_state.group_reads == 1u);
    assert(publication->copy_count == copies + 1u);
    assert(publication->switch_count == switches + 1u);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = payload,
        .payload_capacity = sizeof(payload),
    };
    size_t event_count = 0u;
    while (h2_runtime_poll_event(runtime, &event) == H2_PAL_OK) {
        assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_ERROR);
        event_count += 1u;
    }
    assert(event_count == H2_RUNTIME_DEFAULT_INPUT_SOURCE_CAPACITY);

    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

static void test_radio_state_and_transition_batches_use_one_switch(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_radio_buttons(&env);
    h2_runtime_t *runtime = concurrency_runtime_create(&env);
    h2_runtime_state_publication_t *publication =
        &runtime->private_state->state_publication;
    uint64_t copies = publication->copy_count;
    uint64_t switches = publication->switch_count;
    env.button_state.group_reads = 0u;

    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(env.button_state.group_reads == 1u);
    assert(publication->copy_count == copies);
    assert(publication->switch_count == switches);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = payload,
        .payload_capacity = sizeof(payload),
    };
    assert(h2_runtime_poll_event(runtime, &event) ==
           H2_PAL_ERR_WOULD_BLOCK);

    env.button_state.radio_pressed_id = 1u;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    assert(h2_runtime_poll_event(runtime, &event) ==
           H2_PAL_ERR_WOULD_BLOCK);

    env.button_state.radio_pressed_id = 2u;
    atomic_store(
        &env.time_state.now_ms,
        H2_RUNTIME_BUTTON_POLL_INTERVAL_MS * 2u);
    switches = publication->switch_count;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(publication->switch_count == switches + 1u);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(event.component_id == 1u);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    assert(event.component_id == 1u);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    assert(event.component_id == 2u);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    assert(event.component_id == 2u);
    assert(h2_runtime_poll_event(runtime, &event) ==
           H2_PAL_ERR_WOULD_BLOCK);

    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

/*
 * The station snapshot is published by one writer and polled by readers that
 * never lock. A reader must always come away with one coherent moment, even
 * while publications keep arriving: station events land in bursts, so a
 * scheme that only retires the previous copy would hand a reader a snapshot
 * that is being rewritten underneath it.
 */
typedef struct station_reader_args {
    h2_runtime_t *runtime;
    atomic_int stop;
    unsigned long reads;
    int torn;
} station_reader_args_t;

static void *station_reader_main(void *ctx) {
    station_reader_args_t *args = ctx;
    while (atomic_load(&args->stop) == 0) {
        h2_runtime_system_wifi_sta_state_t state;
        memset(&state, 0, sizeof(state));
        if (h2_runtime_system_state_wifi_sta(args->runtime, &state) != H2_PAL_OK) {
            continue;
        }
        args->reads++;
        if (state.valid == 0u) {
            continue;
        }
        /*
         * Every published snapshot pairs a channel with the matching RSSI and
         * SSID length, so any other combination is a torn read.
         */
        if ((size_t)state.channel != state.ssid_len ||
            state.rssi != -(int32_t)state.channel) {
            args->torn = 1;
        }
    }
    return NULL;
}

static void test_station_snapshot_survives_a_publication_burst(void) {
    concurrency_env_t env;
    concurrency_env_init(&env);
    add_single_button(&env);
    h2_runtime_t *runtime = concurrency_runtime_create(&env);

    station_reader_args_t args = { .runtime = runtime };
    atomic_init(&args.stop, 0);
    pthread_t reader;
    assert(pthread_create(&reader, NULL, station_reader_main, &args) == 0);

    for (unsigned int i = 0u; i < 20000u; ++i) {
        uint8_t channel = (uint8_t)(1u + (i % 13u));
        h2_runtime_system_wifi_sta_state_t state;
        memset(&state, 0, sizeof(state));
        state.valid = 1u;
        state.status = H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTED;
        state.channel = channel;
        state.ssid_len = channel;
        state.rssi = -(int32_t)channel;
        memset(state.ssid, 'a', channel);
        assert(h2_runtime_test_set_system_wifi_sta_state(runtime, &state) ==
               H2_PAL_OK);
    }

    atomic_store(&args.stop, 1);
    assert(pthread_join(reader, NULL) == 0);
    assert(args.reads > 0u);
    assert(args.torn == 0);

    /* The last publication is what a later reader sees. */
    h2_runtime_system_wifi_sta_state_t final_state;
    assert(h2_runtime_system_state_wifi_sta(runtime, &final_state) == H2_PAL_OK);
    assert(final_state.valid == 1u);
    assert((size_t)final_state.channel == final_state.ssid_len);

    h2_runtime_deinit(runtime);
    concurrency_env_deinit(&env);
}

int main(void) {
    test_station_snapshot_survives_a_publication_burst();
    test_slow_pal_does_not_block_snapshot_read();
    test_push_edge_does_not_write_while_poller_is_blocked();
    test_pinned_reader_does_not_block_publication();
    test_restart_keeps_publication_and_reader_pins_valid();
    test_publication_defers_when_all_retired_slots_are_pinned();
    test_publication_counts_and_event_ceiling();
    test_radio_error_batch_uses_one_switch();
    test_radio_state_and_transition_batches_use_one_switch();
    return 0;
}
