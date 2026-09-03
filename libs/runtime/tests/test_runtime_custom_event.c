#define _POSIX_C_SOURCE 200809L

#include "h2_runtime_internal.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CUSTOM_EVENT_OWNER 0x0106u
#define CUSTOM_EVENT_JOB_COMPLETED \
    H2_RUNTIME_CUSTOM_EVENT_ID(CUSTOM_EVENT_OWNER, 1u)
#define CUSTOM_EVENT_JOB_PROGRESS \
    H2_RUNTIME_CUSTOM_EVENT_ID(CUSTOM_EVENT_OWNER, 2u)

#define PRODUCER_COUNT 4u
#define EVENTS_PER_PRODUCER 200u

typedef struct job_completion_event {
    uint32_t job_id;
    uint32_t generation;
    h2_pal_result_t result;
} job_completion_event_t;

typedef struct producer_event {
    uint32_t producer;
    uint32_t index;
    unsigned char pattern[32];
} producer_event_t;

struct h2_pal_mutex {
    pthread_mutex_t native;
    const h2_pal_mem_api_t *mem;
};

struct h2_pal_cond {
    pthread_cond_t native;
    const h2_pal_mem_api_t *mem;
};

struct h2_pal_task {
    pthread_t thread;
    h2_pal_task_entry_t entry;
    void *ctx;
};

struct h2_pal_queue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    size_t item_size;
    size_t item_count;
    size_t count;
    size_t head;
    size_t tail;
    size_t senders_waiting;
    int closed;
    uint8_t *items;
    const h2_pal_mem_api_t *mem;
};

typedef struct custom_event_env {
    h2_pal_mem_api_t mem;
    h2_pal_time_api_t time;
    h2_pal_sync_api_t sync;
    h2_pal_task_api_t task;
    h2_pal_queue_api_t queue;
    h2_pal_periph_api_t periph;
    h2_runtime_component_mapper_t mapper;
    h2_pal_queue_t *event_queue;
} custom_event_env_t;

static void *env_alloc(void *user, size_t len) {
    (void)user;
    return calloc(1u, len);
}

static void env_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static h2_pal_result_t env_now(void *user, uint64_t *out_ms) {
    (void)user;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return H2_PAL_ERR_IO;
    }
    *out_ms = (uint64_t)now.tv_sec * 1000u +
              (uint64_t)(now.tv_nsec / 1000000L);
    return H2_PAL_OK;
}

static h2_pal_result_t env_sleep(void *user, uint32_t ms) {
    (void)user;
    struct timespec request = {
        .tv_sec = (time_t)(ms / 1000u),
        .tv_nsec = (long)(ms % 1000u) * 1000000L,
    };
    while (nanosleep(&request, &request) != 0) {
        if (errno != EINTR) {
            return H2_PAL_ERR_IO;
        }
    }
    return H2_PAL_OK;
}

static uint64_t now_ms(void) {
    uint64_t value = 0u;
    assert(env_now(NULL, &value) == H2_PAL_OK);
    return value;
}

static h2_pal_result_t env_mutex_create(
    void *user,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    (void)user;
    h2_pal_mutex_t *mutex = (h2_pal_mutex_t *)h2_pal_mem_alloc(
        config->allocator, sizeof(*mutex));
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

static h2_pal_result_t env_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_destroy(&mutex->native) == 0);
    h2_pal_mem_free(mutex->mem, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t env_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    return pthread_mutex_lock(&mutex->native) == 0 ? H2_PAL_OK
                                                   : H2_PAL_ERR_IO;
}

static h2_pal_result_t env_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    return pthread_mutex_unlock(&mutex->native) == 0 ? H2_PAL_OK
                                                     : H2_PAL_ERR_IO;
}

static h2_pal_result_t env_cond_create(
    void *user,
    const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
    (void)user;
    h2_pal_cond_t *cond =
        (h2_pal_cond_t *)h2_pal_mem_alloc(config->allocator, sizeof(*cond));
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

static h2_pal_result_t env_cond_destroy(void *user, h2_pal_cond_t *cond) {
    (void)user;
    assert(pthread_cond_destroy(&cond->native) == 0);
    h2_pal_mem_free(cond->mem, cond);
    return H2_PAL_OK;
}

static h2_pal_result_t env_cond_wait(
    void *user,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    return pthread_cond_wait(&cond->native, &mutex->native) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_IO;
}

static h2_pal_result_t env_cond_signal(void *user, h2_pal_cond_t *cond) {
    (void)user;
    return pthread_cond_signal(&cond->native) == 0 ? H2_PAL_OK
                                                   : H2_PAL_ERR_IO;
}

static h2_pal_result_t env_cond_broadcast(void *user, h2_pal_cond_t *cond) {
    (void)user;
    return pthread_cond_broadcast(&cond->native) == 0 ? H2_PAL_OK
                                                      : H2_PAL_ERR_IO;
}

static void *task_trampoline(void *user) {
    h2_pal_task_t *task = (h2_pal_task_t *)user;
    task->entry(task->ctx);
    return NULL;
}

static h2_pal_result_t env_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    (void)user;
    (void)options;
    h2_pal_task_t *task = (h2_pal_task_t *)calloc(1u, sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    task->entry = entry;
    task->ctx = ctx;
    if (pthread_create(&task->thread, NULL, task_trampoline, task) != 0) {
        free(task);
        return H2_PAL_ERR_TASK;
    }
    *out_task = task;
    return H2_PAL_OK;
}

static h2_pal_result_t env_task_join(void *user, h2_pal_task_t *task) {
    (void)user;
    assert(pthread_join(task->thread, NULL) == 0);
    free(task);
    return H2_PAL_OK;
}

static int queue_create(
    void *user,
    const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
    custom_event_env_t *env = (custom_event_env_t *)user;
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
    assert(pthread_mutex_init(&queue->mutex, NULL) == 0);
    assert(pthread_cond_init(&queue->not_empty, NULL) == 0);
    assert(pthread_cond_init(&queue->not_full, NULL) == 0);
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    queue->count = 0u;
    queue->head = 0u;
    queue->tail = 0u;
    queue->senders_waiting = 0u;
    queue->closed = 0;
    queue->mem = config->allocator;
    if (env != NULL && env->event_queue == NULL) {
        /* The first queue Runtime creates is the event queue. */
        env->event_queue = queue;
    }
    *out_queue = queue;
    return H2_PAL_OK;
}

static void queue_destroy(void *user, h2_pal_queue_t *queue) {
    custom_event_env_t *env = (custom_event_env_t *)user;
    if (env != NULL && env->event_queue == queue) {
        env->event_queue = NULL;
    }
    assert(pthread_cond_destroy(&queue->not_full) == 0);
    assert(pthread_cond_destroy(&queue->not_empty) == 0);
    assert(pthread_mutex_destroy(&queue->mutex) == 0);
    const h2_pal_mem_api_t *mem = queue->mem;
    h2_pal_mem_free(mem, queue->items);
    h2_pal_mem_free(mem, queue);
}

static int queue_deadline(uint32_t timeout_ms, struct timespec *out_deadline) {
    if (clock_gettime(CLOCK_REALTIME, out_deadline) != 0) {
        return 0;
    }
    out_deadline->tv_sec += (time_t)(timeout_ms / 1000u);
    out_deadline->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (out_deadline->tv_nsec >= 1000000000L) {
        out_deadline->tv_sec += 1;
        out_deadline->tv_nsec -= 1000000000L;
    }
    return 1;
}

static int queue_send(
    void *user,
    h2_pal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms) {
    (void)user;
    struct timespec deadline;
    int bounded = timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER &&
                  timeout_ms != H2_PAL_QUEUE_NO_WAIT;
    if (bounded && !queue_deadline(timeout_ms, &deadline)) {
        return H2_PAL_ERR_IO;
    }

    assert(pthread_mutex_lock(&queue->mutex) == 0);
    int rc = H2_PAL_OK;
    while (queue->closed == 0 && queue->count == queue->item_count) {
        if (timeout_ms == H2_PAL_QUEUE_NO_WAIT) {
            rc = H2_PAL_ERR_FULL;
            break;
        }
        queue->senders_waiting += 1u;
        int wait_rc =
            bounded ? pthread_cond_timedwait(
                          &queue->not_full, &queue->mutex, &deadline)
                    : pthread_cond_wait(&queue->not_full, &queue->mutex);
        queue->senders_waiting -= 1u;
        if (wait_rc == ETIMEDOUT) {
            rc = H2_PAL_ERR_TIMEOUT;
            break;
        }
        assert(wait_rc == 0);
    }
    if (rc == H2_PAL_OK) {
        if (queue->closed != 0) {
            rc = H2_PAL_ERR_CLOSED;
        } else {
            memcpy(
                queue->items + queue->tail * queue->item_size,
                item,
                queue->item_size);
            queue->tail = (queue->tail + 1u) % queue->item_count;
            queue->count += 1u;
            assert(pthread_cond_signal(&queue->not_empty) == 0);
        }
    }
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return rc;
}

static int queue_recv(
    void *user,
    h2_pal_queue_t *queue,
    void *out_item,
    uint32_t timeout_ms) {
    (void)user;
    struct timespec deadline;
    int bounded = timeout_ms != H2_PAL_QUEUE_WAIT_FOREVER &&
                  timeout_ms != H2_PAL_QUEUE_NO_WAIT;
    if (bounded && !queue_deadline(timeout_ms, &deadline)) {
        return H2_PAL_ERR_IO;
    }

    assert(pthread_mutex_lock(&queue->mutex) == 0);
    int rc = H2_PAL_OK;
    while (queue->closed == 0 && queue->count == 0u) {
        if (timeout_ms == H2_PAL_QUEUE_NO_WAIT) {
            rc = H2_PAL_ERR_WOULD_BLOCK;
            break;
        }
        int wait_rc =
            bounded ? pthread_cond_timedwait(
                          &queue->not_empty, &queue->mutex, &deadline)
                    : pthread_cond_wait(&queue->not_empty, &queue->mutex);
        if (wait_rc == ETIMEDOUT) {
            rc = H2_PAL_ERR_TIMEOUT;
            break;
        }
        assert(wait_rc == 0);
    }
    if (rc == H2_PAL_OK) {
        if (queue->count == 0u) {
            rc = H2_PAL_ERR_CLOSED;
        } else {
            memcpy(
                out_item,
                queue->items + queue->head * queue->item_size,
                queue->item_size);
            queue->head = (queue->head + 1u) % queue->item_count;
            queue->count -= 1u;
            assert(pthread_cond_signal(&queue->not_full) == 0);
        }
    }
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return rc;
}

static int queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    queue->closed = 1;
    assert(pthread_cond_broadcast(&queue->not_empty) == 0);
    assert(pthread_cond_broadcast(&queue->not_full) == 0);
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return H2_PAL_OK;
}

static size_t queue_senders_waiting(h2_pal_queue_t *queue) {
    assert(pthread_mutex_lock(&queue->mutex) == 0);
    size_t waiting = queue->senders_waiting;
    assert(pthread_mutex_unlock(&queue->mutex) == 0);
    return waiting;
}

static h2_pal_result_t env_periph_list(
    void *user,
    h2_pal_periph_type_t filter,
    h2_pal_periph_cb_t cb,
    void *cb_user) {
    (void)user;
    (void)filter;
    (void)cb;
    (void)cb_user;
    return H2_PAL_OK;
}

static h2_pal_result_t env_periph_get(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_periph_info_t *out_info) {
    (void)user;
    (void)id;
    (void)out_info;
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t env_mapper_list(
    void *user,
    h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t cb,
    void *cb_user) {
    (void)user;
    (void)filter;
    (void)cb;
    (void)cb_user;
    return H2_PAL_OK;
}

static void env_init(custom_event_env_t *env) {
    memset(env, 0, sizeof(*env));
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = env_alloc,
        .free = env_free,
    };
    env->mem = (h2_pal_mem_api_t){ .vtable = &mem_vtable };
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = env_now,
        .sleep_ms = env_sleep,
    };
    env->time = (h2_pal_time_api_t){ .vtable = &time_vtable };
    static const h2_pal_sync_vtable_t sync_vtable = {
        .create_mutex = env_mutex_create,
        .destroy_mutex = env_mutex_destroy,
        .lock_mutex = env_mutex_lock,
        .unlock_mutex = env_mutex_unlock,
        .create_cond = env_cond_create,
        .destroy_cond = env_cond_destroy,
        .wait_cond = env_cond_wait,
        .signal_cond = env_cond_signal,
        .broadcast_cond = env_cond_broadcast,
    };
    env->sync = (h2_pal_sync_api_t){ .vtable = &sync_vtable };
    static const h2_pal_task_vtable_t task_vtable = {
        .start = env_task_start,
        .join = env_task_join,
    };
    env->task = (h2_pal_task_api_t){ .vtable = &task_vtable };
    static const h2_pal_queue_vtable_t queue_vtable = {
        .create = queue_create,
        .destroy = queue_destroy,
        .send = queue_send,
        .recv = queue_recv,
        .close = queue_close,
    };
    env->queue = (h2_pal_queue_api_t){ .user = env, .vtable = &queue_vtable };
    static const h2_pal_periph_vtable_t periph_vtable = {
        .list = env_periph_list,
        .get = env_periph_get,
    };
    env->periph = (h2_pal_periph_api_t){ .vtable = &periph_vtable };
    static const h2_runtime_component_mapper_vtable_t mapper_vtable = {
        .list = env_mapper_list,
    };
    env->mapper =
        (h2_runtime_component_mapper_t){ .vtable = &mapper_vtable };
}

static h2_runtime_config_t env_runtime_config(custom_event_env_t *env) {
    return (h2_runtime_config_t){
        .board = "custom-event-test",
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
        .system_event = h2_pal_unsupported_system_event_api(),
        .component_mapper = &env->mapper,
        .event_queue_capacity = 64u,
    };
}

static h2_runtime_t *env_runtime_create(
    custom_event_env_t *env,
    size_t event_queue_capacity) {
    h2_runtime_config_t config = env_runtime_config(env);
    config.event_queue_capacity = event_queue_capacity;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime != NULL);
    return runtime;
}

static h2_pal_result_t post_value(
    h2_runtime_t *runtime,
    h2_runtime_custom_event_id_t id,
    const void *payload,
    size_t payload_size) {
    const h2_runtime_custom_event_t event = {
        .id = id,
        .payload = payload,
        .payload_size = payload_size,
    };
    return h2_runtime_post_custom_event(runtime, &event);
}

static const h2_runtime_custom_event_payload_t *custom_payload(
    const h2_runtime_event_t *event) {
    assert(event->kind == H2_RUNTIME_EVENT_CUSTOM);
    assert(event->component == H2_RUNTIME_COMPONENT_APP);
    assert(event->component_id == H2_RUNTIME_COMPONENT_ID_NONE);
    assert(event->sequence != 0u);
    assert(event->payload_size >= H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE);
    return (const h2_runtime_custom_event_payload_t *)event->payload;
}

typedef struct waiter_state {
    h2_runtime_t *runtime;
    h2_pal_result_t result;
    h2_runtime_event_payload_buffer_t buffer;
    h2_runtime_event_t event;
    uint64_t woke_at_ms;
} waiter_state_t;

static void *waiter_thread(void *user) {
    waiter_state_t *state = (waiter_state_t *)user;
    state->event = (h2_runtime_event_t){
        .payload = state->buffer.bytes,
        .payload_capacity = sizeof(state->buffer.bytes),
    };
    state->result = h2_runtime_wait_event(
        state->runtime, &state->event, H2_PAL_QUEUE_WAIT_FOREVER);
    state->woke_at_ms = now_ms();
    return NULL;
}

/* A custom event must wake a consumer parked in an indefinite wait. */
static void test_custom_event_wakes_indefinite_wait(void) {
    custom_event_env_t env;
    env_init(&env);
    h2_runtime_t *runtime = env_runtime_create(&env, 8u);

    waiter_state_t state = { .runtime = runtime, .result = H2_PAL_ERR_IO };
    pthread_t waiter;
    assert(pthread_create(&waiter, NULL, waiter_thread, &state) == 0);

    /* Let the consumer reach the blocking wait before anything is posted. */
    assert(env_sleep(NULL, 50u) == H2_PAL_OK);
    const job_completion_event_t completion = {
        .job_id = 7u,
        .generation = 3u,
        .result = H2_PAL_OK,
    };
    uint64_t posted_at_ms = now_ms();
    assert(post_value(
               runtime,
               CUSTOM_EVENT_JOB_COMPLETED,
               &completion,
               sizeof(completion)) == H2_PAL_OK);
    assert(pthread_join(waiter, NULL) == 0);

    assert(state.result == H2_PAL_OK);
    /* Woken by the post, not by a poll interval. */
    assert(state.woke_at_ms - posted_at_ms < 1000u);
    const h2_runtime_custom_event_payload_t *payload =
        custom_payload(&state.event);
    assert(payload->id == CUSTOM_EVENT_JOB_COMPLETED);
    assert(H2_RUNTIME_CUSTOM_EVENT_ID_OWNER(payload->id) ==
           CUSTOM_EVENT_OWNER);
    assert(H2_RUNTIME_CUSTOM_EVENT_ID_EVENT(payload->id) == 1u);
    assert(payload->size == sizeof(completion));
    job_completion_event_t received;
    memcpy(&received, payload->data, sizeof(received));
    assert(received.job_id == 7u);
    assert(received.generation == 3u);
    assert(received.result == H2_PAL_OK);

    h2_runtime_deinit(runtime);
}

/* Runtime copies the payload: the poster may reuse its buffer immediately. */
static void test_custom_event_payload_is_copied(void) {
    custom_event_env_t env;
    env_init(&env);
    h2_runtime_t *runtime = env_runtime_create(&env, 8u);

    job_completion_event_t completion = {
        .job_id = 11u,
        .generation = 2u,
        .result = H2_PAL_ERR_TIMEOUT,
    };
    assert(post_value(
               runtime,
               CUSTOM_EVENT_JOB_COMPLETED,
               &completion,
               sizeof(completion)) == H2_PAL_OK);
    completion = (job_completion_event_t){
        .job_id = 0xdeadbeefu,
        .generation = 0xffffffffu,
        .result = H2_PAL_ERR_IO,
    };

    h2_runtime_event_payload_buffer_t buffer;
    h2_runtime_event_t event = {
        .payload = buffer.bytes,
        .payload_capacity = sizeof(buffer.bytes),
    };
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    const h2_runtime_custom_event_payload_t *payload = custom_payload(&event);
    job_completion_event_t received;
    memcpy(&received, payload->data, sizeof(received));
    assert(received.job_id == 11u);
    assert(received.generation == 2u);
    assert(received.result == H2_PAL_ERR_TIMEOUT);

    h2_runtime_deinit(runtime);
}

/* Empty payloads are legal, ids stay opaque and FIFO order is preserved. */
static void test_custom_events_keep_fifo_order(void) {
    custom_event_env_t env;
    env_init(&env);
    h2_runtime_t *runtime = env_runtime_create(&env, 8u);

    for (uint32_t index = 0u; index < 4u; ++index) {
        assert(post_value(
                   runtime,
                   CUSTOM_EVENT_JOB_PROGRESS,
                   &index,
                   sizeof(index)) == H2_PAL_OK);
    }
    assert(post_value(runtime, CUSTOM_EVENT_JOB_COMPLETED, NULL, 0u) ==
           H2_PAL_OK);

    h2_runtime_event_payload_buffer_t buffer;
    h2_runtime_sequence_t previous_sequence = 0u;
    for (uint32_t index = 0u; index < 4u; ++index) {
        h2_runtime_event_t event = {
            .payload = buffer.bytes,
            .payload_capacity = sizeof(buffer.bytes),
        };
        assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
        const h2_runtime_custom_event_payload_t *payload =
            custom_payload(&event);
        assert(payload->id == CUSTOM_EVENT_JOB_PROGRESS);
        assert(payload->size == sizeof(index));
        uint32_t received = 0u;
        memcpy(&received, payload->data, sizeof(received));
        assert(received == index);
        assert(event.sequence > previous_sequence);
        previous_sequence = event.sequence;
    }

    h2_runtime_event_t event = {
        .payload = buffer.bytes,
        .payload_capacity = sizeof(buffer.bytes),
    };
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    const h2_runtime_custom_event_payload_t *payload = custom_payload(&event);
    assert(payload->id == CUSTOM_EVENT_JOB_COMPLETED);
    assert(payload->size == 0u);
    assert(event.payload_size == H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);

    h2_runtime_deinit(runtime);
}

/* A full queue is reported, never silently dropped. */
static void test_custom_event_reports_full_queue(void) {
    custom_event_env_t env;
    env_init(&env);
    h2_runtime_t *runtime = env_runtime_create(&env, 2u);

    uint32_t value = 1u;
    assert(post_value(runtime, CUSTOM_EVENT_JOB_PROGRESS, &value,
                      sizeof(value)) == H2_PAL_OK);
    assert(post_value(runtime, CUSTOM_EVENT_JOB_PROGRESS, &value,
                      sizeof(value)) == H2_PAL_OK);
    assert(post_value(runtime, CUSTOM_EVENT_JOB_PROGRESS, &value,
                      sizeof(value)) == H2_PAL_ERR_FULL);
    /* The bounded variant reports the same backpressure without hanging. */
    const h2_runtime_custom_event_t event = {
        .id = CUSTOM_EVENT_JOB_PROGRESS,
        .payload = &value,
        .payload_size = sizeof(value),
    };
    h2_pal_result_t rc =
        h2_runtime_post_custom_event_timeout(runtime, &event, 20u);
    assert(rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_FULL);

    h2_runtime_event_payload_buffer_t buffer;
    h2_runtime_event_t out = {
        .payload = buffer.bytes,
        .payload_capacity = sizeof(buffer.bytes),
    };
    assert(h2_runtime_poll_event(runtime, &out) == H2_PAL_OK);
    assert(post_value(runtime, CUSTOM_EVENT_JOB_PROGRESS, &value,
                      sizeof(value)) == H2_PAL_OK);

    h2_runtime_deinit(runtime);
}

static void test_custom_event_rejects_invalid_requests(void) {
    custom_event_env_t env;
    env_init(&env);
    h2_runtime_t *runtime = env_runtime_create(&env, 8u);

    size_t capacity = 0u;
    assert(h2_runtime_custom_event_payload_capacity(runtime, &capacity) ==
           H2_PAL_OK);
    assert(capacity == H2_RUNTIME_CUSTOM_EVENT_PAYLOAD_MAX);
    assert(h2_runtime_custom_event_payload_capacity(runtime, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_runtime_custom_event_payload_capacity(NULL, &capacity) ==
           H2_PAL_ERR_INVALID_ARG);

    static unsigned char oversized[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    assert(post_value(
               runtime,
               CUSTOM_EVENT_JOB_PROGRESS,
               oversized,
               capacity + 1u) == H2_PAL_ERR_TRUNCATED);
    assert(post_value(runtime, CUSTOM_EVENT_JOB_PROGRESS, NULL, 4u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_runtime_post_custom_event(runtime, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_runtime_post_custom_event(NULL, NULL) ==
           H2_PAL_ERR_INVALID_ARG);

    const h2_runtime_custom_event_t event = {
        .id = CUSTOM_EVENT_JOB_PROGRESS,
        .payload = oversized,
        .payload_size = 4u,
    };
    assert(h2_runtime_post_custom_event_timeout(
               runtime, &event, H2_PAL_QUEUE_WAIT_FOREVER) ==
           H2_PAL_ERR_INVALID_ARG);
    /* A payload that exactly fills the capacity is accepted. */
    assert(post_value(
               runtime, CUSTOM_EVENT_JOB_PROGRESS, oversized, capacity) ==
           H2_PAL_OK);
    h2_runtime_event_payload_buffer_t buffer;
    h2_runtime_event_t out = {
        .payload = buffer.bytes,
        .payload_capacity = sizeof(buffer.bytes),
    };
    assert(h2_runtime_poll_event(runtime, &out) == H2_PAL_OK);
    assert(custom_payload(&out)->size == capacity);

    h2_runtime_deinit(runtime);
}

/* A smaller configured payload capacity shrinks the custom payload budget. */
static void test_custom_event_respects_configured_capacity(void) {
    custom_event_env_t env;
    env_init(&env);
    h2_runtime_config_t config = env_runtime_config(&env);
    const size_t payload_capacity =
        h2_runtime_system_event_payload_capacity_min();
    config.event_payload_capacity = payload_capacity;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);

    size_t capacity = 0u;
    assert(h2_runtime_custom_event_payload_capacity(runtime, &capacity) ==
           H2_PAL_OK);
    assert(capacity ==
           payload_capacity - H2_RUNTIME_CUSTOM_EVENT_HEADER_SIZE);
    static unsigned char payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    assert(post_value(
               runtime, CUSTOM_EVENT_JOB_PROGRESS, payload, capacity + 1u) ==
           H2_PAL_ERR_TRUNCATED);
    assert(post_value(
               runtime, CUSTOM_EVENT_JOB_PROGRESS, payload, capacity) ==
           H2_PAL_OK);

    h2_runtime_deinit(runtime);
}

typedef struct producer_state {
    h2_runtime_t *runtime;
    uint32_t producer;
    size_t posted;
} producer_state_t;

static void *producer_thread(void *user) {
    producer_state_t *state = (producer_state_t *)user;
    for (uint32_t index = 0u; index < EVENTS_PER_PRODUCER; ++index) {
        producer_event_t value = {
            .producer = state->producer,
            .index = index,
        };
        for (size_t byte = 0u; byte < sizeof(value.pattern); ++byte) {
            value.pattern[byte] =
                (unsigned char)(state->producer * 31u + index + byte);
        }
        const h2_runtime_custom_event_t event = {
            .id = CUSTOM_EVENT_JOB_PROGRESS,
            .payload = &value,
            .payload_size = sizeof(value),
        };
        for (;;) {
            h2_pal_result_t rc =
                h2_runtime_post_custom_event_timeout(
                    state->runtime, &event, 100u);
            if (rc == H2_PAL_OK) {
                state->posted += 1u;
                break;
            }
            /* Only backpressure is retried; anything else fails the test. */
            assert(rc == H2_PAL_ERR_FULL || rc == H2_PAL_ERR_TIMEOUT);
        }
    }
    return NULL;
}

/* Concurrent producers lose nothing and never corrupt a payload. */
static void test_concurrent_producers_preserve_every_event(void) {
    custom_event_env_t env;
    env_init(&env);
    h2_runtime_t *runtime = env_runtime_create(&env, 8u);

    producer_state_t states[PRODUCER_COUNT];
    pthread_t producers[PRODUCER_COUNT];
    for (uint32_t producer = 0u; producer < PRODUCER_COUNT; ++producer) {
        states[producer] = (producer_state_t){
            .runtime = runtime,
            .producer = producer,
        };
        assert(pthread_create(
                   &producers[producer],
                   NULL,
                   producer_thread,
                   &states[producer]) == 0);
    }

    uint32_t next_index[PRODUCER_COUNT] = { 0u };
    size_t received = 0u;
    h2_runtime_event_payload_buffer_t buffer;
    while (received < PRODUCER_COUNT * EVENTS_PER_PRODUCER) {
        h2_runtime_event_t event = {
            .payload = buffer.bytes,
            .payload_capacity = sizeof(buffer.bytes),
        };
        h2_pal_result_t rc = h2_runtime_wait_event(runtime, &event, 5000u);
        assert(rc == H2_PAL_OK);
        const h2_runtime_custom_event_payload_t *payload =
            custom_payload(&event);
        assert(payload->id == CUSTOM_EVENT_JOB_PROGRESS);
        assert(payload->size == sizeof(producer_event_t));
        producer_event_t value;
        memcpy(&value, payload->data, sizeof(value));
        assert(value.producer < PRODUCER_COUNT);
        /* Per-producer FIFO: a producer's events arrive in the order sent. */
        assert(value.index == next_index[value.producer]);
        for (size_t byte = 0u; byte < sizeof(value.pattern); ++byte) {
            assert(value.pattern[byte] ==
                   (unsigned char)(value.producer * 31u + value.index + byte));
        }
        next_index[value.producer] += 1u;
        received += 1u;
    }

    for (uint32_t producer = 0u; producer < PRODUCER_COUNT; ++producer) {
        assert(pthread_join(producers[producer], NULL) == 0);
        assert(states[producer].posted == EVENTS_PER_PRODUCER);
        assert(next_index[producer] == EVENTS_PER_PRODUCER);
    }

    h2_runtime_deinit(runtime);
}

typedef struct blocked_producer_state {
    h2_runtime_t *runtime;
    h2_pal_result_t result;
} blocked_producer_state_t;

static void *blocked_producer_thread(void *user) {
    blocked_producer_state_t *state = (blocked_producer_state_t *)user;
    uint32_t value = 42u;
    const h2_runtime_custom_event_t event = {
        .id = CUSTOM_EVENT_JOB_COMPLETED,
        .payload = &value,
        .payload_size = sizeof(value),
    };
    state->result =
        h2_runtime_post_custom_event_timeout(state->runtime, &event, 30000u);
    return NULL;
}

/*
 * Deinit must release and drain a poster that is still inside a bounded post,
 * so the event queue is never destroyed under it.
 */
static void test_deinit_drains_in_flight_poster(void) {
    custom_event_env_t env;
    env_init(&env);
    h2_runtime_t *runtime = env_runtime_create(&env, 1u);

    uint32_t value = 1u;
    assert(post_value(runtime, CUSTOM_EVENT_JOB_PROGRESS, &value,
                      sizeof(value)) == H2_PAL_OK);

    blocked_producer_state_t state = {
        .runtime = runtime,
        .result = H2_PAL_OK,
    };
    pthread_t producer;
    assert(pthread_create(
               &producer, NULL, blocked_producer_thread, &state) == 0);
    assert(env.event_queue != NULL);
    while (queue_senders_waiting(env.event_queue) == 0u) {
        assert(env_sleep(NULL, 1u) == H2_PAL_OK);
    }

    uint64_t started_ms = now_ms();
    h2_runtime_deinit(runtime);
    assert(now_ms() - started_ms < 5000u);
    assert(pthread_join(producer, NULL) == 0);
    assert(state.result == H2_PAL_ERR_CLOSED);
}

int main(void) {
    test_custom_event_wakes_indefinite_wait();
    test_custom_event_payload_is_copied();
    test_custom_events_keep_fifo_order();
    test_custom_event_reports_full_queue();
    test_custom_event_rejects_invalid_requests();
    test_custom_event_respects_configured_capacity();
    test_concurrent_producers_preserve_every_event();
    test_deinit_drains_in_flight_poster();
    return 0;
}
