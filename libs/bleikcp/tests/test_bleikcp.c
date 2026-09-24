#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_test_allocator.h"
#include "h2_bleikcp_internal.h"
#include "h2_bleikcp_task_names.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

#define TEST_IO_TIMEOUT_MS 15000u

struct h2_pal_mutex { pthread_mutex_t value; const h2_pal_mem_api_t *allocator; };
struct h2_pal_cond { pthread_cond_t value; const h2_pal_mem_api_t *allocator; };
struct h2_pal_task { pthread_t thread; h2_pal_task_entry_t entry; void *ctx; };
struct h2_pal_system_event_subscription {
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *user;
};

typedef struct fake_runtime {
    pthread_mutex_t event_mutex;
    h2_pal_system_event_subscription_t *subscriptions[16];
    size_t subscription_count;
    const h2_pal_ble_gatt_service_t *service;
    size_t service_count;
    int unregister_count;
    int unregister_service_count;
    h2_pal_result_t unregister_service_result;
    h2_atomic_int_t fail_next_register;
    h2_atomic_int_t fail_next_join;
    h2_atomic_int_t drop_next_gatt_write;
    h2_atomic_int_t cond_broadcasts;
    /* Sleep gate: while armed, every sleep from a thread other than the test
     * main thread parks until released, which holds KCP workers still. */
    pthread_t main_thread;
    pthread_mutex_t gate_mutex;
    pthread_cond_t gate_cond;
    int gate_armed;
    int gate_parked;
    uint32_t rx_properties_override;
    h2_pal_mem_api_t allocator;
    h2_pal_ble_t ble;
    h2_pal_task_api_t task;
    h2_pal_time_api_t time;
    h2_pal_sync_api_t sync;
    h2_pal_system_event_api_t events;
} fake_runtime_t;

static void *fake_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *fake_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void fake_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static const h2_pal_mem_vtable_t s_allocator_vtable = {
    .alloc = fake_alloc,
    .realloc = fake_realloc,
    .free = fake_free,
};

static h2_pal_result_t fake_mutex_create(
    void *user,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    (void)user;
    h2_pal_mutex_t *mutex = h2_pal_mem_alloc(config->allocator, sizeof(*mutex));
    if (mutex == NULL) return H2_PAL_ERR_NO_MEMORY;
    if (pthread_mutex_init(&mutex->value, NULL) != 0) {
        h2_pal_mem_free(config->allocator, mutex);
        return H2_PAL_ERR_IO;
    }
    mutex->allocator = config->allocator;
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    (void)pthread_mutex_destroy(&mutex->value);
    h2_pal_mem_free(mutex->allocator, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    return pthread_mutex_lock(&mutex->value) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t fake_mutex_try_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    int rc = pthread_mutex_trylock(&mutex->value);
    return rc == 0 ? H2_PAL_OK : (rc == EBUSY ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO);
}

static h2_pal_result_t fake_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    return pthread_mutex_unlock(&mutex->value) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t fake_cond_create(
    void *user,
    const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
    (void)user;
    h2_pal_cond_t *cond = h2_pal_mem_alloc(config->allocator, sizeof(*cond));
    if (cond == NULL) return H2_PAL_ERR_NO_MEMORY;
    if (pthread_cond_init(&cond->value, NULL) != 0) {
        h2_pal_mem_free(config->allocator, cond);
        return H2_PAL_ERR_IO;
    }
    cond->allocator = config->allocator;
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_cond_destroy(void *user, h2_pal_cond_t *cond) {
    (void)user;
    (void)pthread_cond_destroy(&cond->value);
    h2_pal_mem_free(cond->allocator, cond);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_cond_wait(
    void *user,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    (void)user;
    if (timeout_ms == H2_PAL_SYNC_WAIT_FOREVER) {
        return pthread_cond_wait(&cond->value, &mutex->value) == 0
                   ? H2_PAL_OK
                   : H2_PAL_ERR_IO;
    }
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000u;
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(&cond->value, &mutex->value, &deadline);
    return rc == 0 ? H2_PAL_OK : (rc == ETIMEDOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO);
}

static h2_pal_result_t fake_cond_signal(void *user, h2_pal_cond_t *cond) {
    (void)user;
    return pthread_cond_signal(&cond->value) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t fake_cond_broadcast(void *user, h2_pal_cond_t *cond) {
    fake_runtime_t *runtime = user;
    h2_atomic_fetch_add(&runtime->cond_broadcasts, 1);
    return pthread_cond_broadcast(&cond->value) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static const h2_pal_sync_vtable_t s_sync_vtable = {
    .create_mutex = fake_mutex_create,
    .destroy_mutex = fake_mutex_destroy,
    .lock_mutex = fake_mutex_lock,
    .try_lock_mutex = fake_mutex_try_lock,
    .unlock_mutex = fake_mutex_unlock,
    .create_cond = fake_cond_create,
    .destroy_cond = fake_cond_destroy,
    .wait_cond = fake_cond_wait,
    .signal_cond = fake_cond_signal,
    .broadcast_cond = fake_cond_broadcast,
};

static void *fake_task_main(void *ctx) {
    h2_pal_task_t *task = ctx;
    task->entry(task->ctx);
    return NULL;
}

static int fake_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    fake_runtime_t *runtime = user;
    (void)options;
    h2_pal_task_t *task = h2_pal_mem_alloc(&runtime->allocator, sizeof(*task));
    if (task == NULL) return H2_PAL_ERR_NO_MEMORY;
    task->entry = entry;
    task->ctx = ctx;
    if (pthread_create(&task->thread, NULL, fake_task_main, task) != 0) {
        h2_pal_mem_free(&runtime->allocator, task);
        return H2_PAL_ERR_TASK;
    }
    *out_task = task;
    return H2_PAL_OK;
}

static int fake_task_join(void *user, h2_pal_task_t *task) {
    fake_runtime_t *runtime = user;
    if (h2_atomic_exchange(&runtime->fail_next_join, 0) != 0) {
        return H2_PAL_ERR_TASK;
    }
    int rc = pthread_join(task->thread, NULL) == 0 ? H2_PAL_OK : H2_PAL_ERR_TASK;
    if (rc == H2_PAL_OK) h2_pal_mem_free(&runtime->allocator, task);
    return rc;
}

static h2_pal_result_t fake_monotonic(void *user, uint64_t *out_ms) {
    (void)user;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return H2_PAL_ERR_IO;
    *out_ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_sleep(void *user, uint32_t ms) {
    fake_runtime_t *runtime = user;
    if (!pthread_equal(pthread_self(), runtime->main_thread)) {
        pthread_mutex_lock(&runtime->gate_mutex);
        if (runtime->gate_armed) {
            runtime->gate_parked++;
            pthread_cond_broadcast(&runtime->gate_cond);
            while (runtime->gate_armed) {
                pthread_cond_wait(&runtime->gate_cond, &runtime->gate_mutex);
            }
            runtime->gate_parked--;
        }
        pthread_mutex_unlock(&runtime->gate_mutex);
    }
    struct timespec delay = {
        .tv_sec = ms / 1000u,
        .tv_nsec = (long)(ms % 1000u) * 1000000L,
    };
    return nanosleep(&delay, NULL) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static const h2_pal_time_vtable_t s_time_vtable = {
    .get_monotonic_ms = fake_monotonic,
    .get_wall_ms = fake_monotonic,
    .sleep_ms = fake_sleep,
};

static int fake_event_subscribe(
    void *user,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    fake_runtime_t *runtime = user;
    h2_pal_system_event_subscription_t *subscription = malloc(sizeof(*subscription));
    if (subscription == NULL) return H2_PAL_ERR_NO_MEMORY;
    *subscription = (h2_pal_system_event_subscription_t){
        .type = type,
        .handler = handler,
        .user = handler_user,
    };
    pthread_mutex_lock(&runtime->event_mutex);
    if (runtime->subscription_count == 16u) {
        pthread_mutex_unlock(&runtime->event_mutex);
        free(subscription);
        return H2_PAL_ERR_FULL;
    }
    runtime->subscriptions[runtime->subscription_count++] = subscription;
    pthread_mutex_unlock(&runtime->event_mutex);
    *out_subscription = subscription;
    return H2_PAL_OK;
}

static void fake_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription) {
    fake_runtime_t *runtime = user;
    pthread_mutex_lock(&runtime->event_mutex);
    for (size_t i = 0u; i < runtime->subscription_count; ++i) {
        if (runtime->subscriptions[i] == subscription) {
            runtime->subscriptions[i] = runtime->subscriptions[--runtime->subscription_count];
            break;
        }
    }
    pthread_mutex_unlock(&runtime->event_mutex);
    free(subscription);
}

static int fake_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms) {
    fake_runtime_t *runtime = user;
    (void)timeout_ms;
    h2_pal_system_event_subscription_t *snapshot[16];
    size_t count = 0u;
    pthread_mutex_lock(&runtime->event_mutex);
    for (size_t i = 0u; i < runtime->subscription_count; ++i) {
        if (runtime->subscriptions[i]->type == event->type) {
            snapshot[count++] = runtime->subscriptions[i];
        }
    }
    pthread_mutex_unlock(&runtime->event_mutex);
    for (size_t i = 0u; i < count; ++i) {
        (void)snapshot[i]->handler(snapshot[i]->user, event);
    }
    return H2_PAL_OK;
}

static int fake_post_payload(
    fake_runtime_t *runtime,
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size) {
    h2_pal_system_event_t event = {
        .type = type,
        .payload = payload,
        .payload_size = payload_size,
    };
    return fake_event_post(runtime, &event, 0u);
}

static h2_pal_result_t fake_register(
    void *user,
    const h2_pal_ble_gatt_service_t *services,
    size_t count) {
    fake_runtime_t *runtime = (fake_runtime_t *)user;
    if (h2_atomic_exchange(&runtime->fail_next_register, 0) != 0) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (count != 1u || services == NULL || services[0].characteristic_count < 2u ||
        services[0].characteristic_count > 2u + H2_BLEIKCP_SERVER_EXTRA_CHARACTERISTIC_MAX) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    runtime->service = services;
    runtime->service_count = count;
    if (services[0].out_service_handle != NULL) *services[0].out_service_handle = 1u;
    for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
        const h2_pal_ble_gatt_characteristic_t *ch = &services[0].characteristics[i];
        if (ch->out_value_handle != NULL) *ch->out_value_handle = (uint16_t)(2u + 2u * i);
        if (ch->out_cccd_handle != NULL) *ch->out_cccd_handle = (uint16_t)(3u + 2u * i);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t fake_unregister_service(
    void *user, const h2_pal_ble_uuid_t *service_uuid) {
    fake_runtime_t *runtime = user;
    CHECK(runtime->service != NULL);
    CHECK(service_uuid != NULL);
    CHECK(service_uuid->len == runtime->service->uuid.len);
    CHECK(memcmp(service_uuid->data, runtime->service->uuid.data,
                 service_uuid->len) == 0);
    runtime->unregister_service_count++;
    if (runtime->unregister_service_result != H2_PAL_OK) {
        return runtime->unregister_service_result;
    }
    runtime->service = NULL;
    runtime->service_count = 0u;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_unregister(void *user) {
    fake_runtime_t *runtime = (fake_runtime_t *)user;
    runtime->service = NULL;
    runtime->service_count = 0u;
    runtime->unregister_count++;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_discover(
    void *user,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t timeout_ms) {
    fake_runtime_t *runtime = (fake_runtime_t *)user;
    (void)conn_handle;
    (void)timeout_ms;
    *out_count = 0u;
    if (runtime->service == NULL || max_entries == 0u) return H2_PAL_ERR_NOT_FOUND;
    const h2_pal_ble_gatt_service_t *service = runtime->service;
    if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_SERVICE) {
        entries[0] = (h2_pal_ble_gatt_discovery_entry_t){
            .kind = request->kind,
            .uuid = service->uuid,
            .start_handle = 1u,
            .end_handle = 5u,
        };
        *out_count = 1u;
        return H2_PAL_OK;
    }
    if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC) {
        for (size_t i = 0u; i < service->characteristic_count; ++i) {
            const h2_pal_ble_gatt_characteristic_t *ch = &service->characteristics[i];
            if (ch->uuid.len == request->uuid_filter.len &&
                memcmp(ch->uuid.data, request->uuid_filter.data, ch->uuid.len) == 0) {
                entries[0] = (h2_pal_ble_gatt_discovery_entry_t){
                    .kind = request->kind,
                    .uuid = ch->uuid,
                    .value_handle = (uint16_t)(2u + 2u * i),
                    .properties = ch->write != NULL &&
                                          runtime->rx_properties_override != 0u
                                      ? runtime->rx_properties_override
                                      : ch->properties,
                };
                *out_count = 1u;
                return H2_PAL_OK;
            }
        }
    } else if (request->kind == H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR) {
        static const uint8_t uuid_data[] = { 0x02u, 0x29u };
        entries[0] = (h2_pal_ble_gatt_discovery_entry_t){
            .kind = request->kind,
            .uuid = { .data = uuid_data, .len = sizeof(uuid_data) },
            .value_handle = 3u,
        };
        *out_count = 1u;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t fake_gatt_write(
    void *user,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    bool with_response,
    uint32_t timeout_ms) {
    fake_runtime_t *runtime = (fake_runtime_t *)user;
    (void)with_response;
    (void)timeout_ms;
    if (runtime->service == NULL) return H2_PAL_ERR_CLOSED;
    if (h2_atomic_exchange(&runtime->drop_next_gatt_write, 0) != 0) {
        return H2_PAL_OK;
    }
    for (size_t i = 0u; i < runtime->service->characteristic_count; ++i) {
        const h2_pal_ble_gatt_characteristic_t *ch = &runtime->service->characteristics[i];
        if ((uint16_t)(2u + 2u * i) == attr_handle && ch->write != NULL) {
            h2_pal_ble_gatt_access_t access = {
                .conn_handle = conn_handle,
                .attr_handle = attr_handle,
            };
            return ch->write(ch->user, &access, data, len);
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t fake_subscribe(
    void *user,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe,
    uint32_t timeout_ms) {
    fake_runtime_t *runtime = (fake_runtime_t *)user;
    (void)timeout_ms;
    h2_pal_ble_subscription_state_t state = {
        .conn_handle = conn_handle,
        .value_handle = subscribe->value_handle,
        .mode = subscribe->mode,
        .enabled = subscribe->enable,
    };
    (void)fake_post_payload(
        runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
        &state, sizeof(state));
    return H2_PAL_OK;
}

static h2_pal_result_t fake_notify(
    void *user,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len) {
    fake_runtime_t *runtime = (fake_runtime_t *)user;
    h2_pal_ble_gatt_client_value_t value;
    memset(&value, 0, sizeof(value));
    value.conn_handle = conn_handle;
    value.attr_handle = attr_handle;
    value.value_len = len < sizeof(value.value) ? len : sizeof(value.value);
    if (value.value_len > 0u) memcpy(value.value, data, value.value_len);
    return fake_post_payload(
        runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
        &value, sizeof(value));
}

static h2_pal_result_t fake_disconnect(void *user, uint16_t conn_handle) {
    fake_runtime_t *runtime = (fake_runtime_t *)user;
    h2_pal_ble_disconnected_info_t info = { .conn_handle = conn_handle };
    (void)fake_post_payload(
        runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
        &info, sizeof(info));
    return H2_PAL_OK;
}

static void fake_runtime_atomics_destroy(fake_runtime_t *runtime) {
    h2_atomic_destroy(&runtime->fail_next_register);
    h2_atomic_destroy(&runtime->fail_next_join);
    h2_atomic_destroy(&runtime->drop_next_gatt_write);
    h2_atomic_destroy(&runtime->cond_broadcasts);
}

static void fake_runtime_init(fake_runtime_t *runtime) {
    memset(runtime, 0, sizeof(*runtime));
    CHECK(h2_atomic_int_init(&runtime->fail_next_register, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&runtime->fail_next_join, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&runtime->drop_next_gatt_write, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&runtime->cond_broadcasts, 0) == H2_ATOMIC_OK);
    CHECK(pthread_mutex_init(&runtime->event_mutex, NULL) == 0);
    runtime->main_thread = pthread_self();
    CHECK(pthread_mutex_init(&runtime->gate_mutex, NULL) == 0);
    CHECK(pthread_cond_init(&runtime->gate_cond, NULL) == 0);
    runtime->allocator = (h2_pal_mem_api_t){
        .user = runtime,
        .vtable = &s_allocator_vtable,
    };
    runtime->sync = (h2_pal_sync_api_t){ .user = runtime, .vtable = &s_sync_vtable };
    static const h2_pal_task_vtable_t task_vtable = {
        .start = fake_task_start,
        .join = fake_task_join,
    };
    runtime->task = (h2_pal_task_api_t){
        .user = runtime,
        .vtable = &task_vtable,
    };
    runtime->time = (h2_pal_time_api_t){ .user = runtime, .vtable = &s_time_vtable };
    static const h2_pal_system_event_vtable_t system_event_vtable = {
        .post = fake_event_post,
        .subscribe = fake_event_subscribe,
        .unsubscribe = fake_event_unsubscribe,
    };
    runtime->events = (h2_pal_system_event_api_t){
        .user = runtime,
        .vtable = &system_event_vtable,
    };
    static const h2_pal_ble_vtable_t ble_vtable = {
        .register_gatt_services = fake_register,
        .unregister_gatt_services = fake_unregister,
        .notify = fake_notify,
        .disconnect = fake_disconnect,
        .gatt_discover = fake_discover,
        .gatt_write = fake_gatt_write,
        .gatt_subscribe = fake_subscribe,
    };
    runtime->ble = (h2_pal_ble_t){
        .user = runtime,
        .vtable = &ble_vtable,
        .allocator = &runtime->allocator,
    };
}

typedef struct handler_state {
    const h2_bleikcp_api_t *api;
    h2_atomic_int_t calls;
    h2_atomic_int_t replies_drained;
} handler_state_t;

typedef struct event_state {
    h2_atomic_int_t connected;
    h2_atomic_int_t ready;
    h2_atomic_int_t client_disconnected;
    h2_atomic_int_t protocol_error;
} event_state_t;

static void handler_state_init(handler_state_t *state) {
    CHECK(h2_atomic_int_init(&state->calls, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&state->replies_drained, 0) == H2_ATOMIC_OK);
}
static void handler_state_destroy(handler_state_t *state) {
    h2_atomic_destroy(&state->calls);
    h2_atomic_destroy(&state->replies_drained);
}
static void event_state_init(event_state_t *state) {
    CHECK(h2_atomic_int_init(&state->connected, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&state->ready, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&state->client_disconnected, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&state->protocol_error, 0) == H2_ATOMIC_OK);
}
static void event_state_destroy(event_state_t *state) {
    h2_atomic_destroy(&state->connected);
    h2_atomic_destroy(&state->ready);
    h2_atomic_destroy(&state->client_disconnected);
    h2_atomic_destroy(&state->protocol_error);
}

static void wait_for_atomic_at_least(
    const h2_bleikcp_api_t *api,
    const h2_atomic_int_t *value,
    int expected,
    const char *name) {
    uint64_t started_ms = 0u;
    CHECK(h2_pal_time_get_monotonic_ms(api->time, &started_ms) == H2_PAL_OK);
    uint64_t deadline_ms = h2_pal_time_deadline_ms(started_ms, TEST_IO_TIMEOUT_MS);
    for (;;) {
        int observed = h2_atomic_load(value);
        if (observed >= expected) return;
        uint64_t now_ms = 0u;
        CHECK(h2_pal_time_get_monotonic_ms(api->time, &now_ms) == H2_PAL_OK);
        if (h2_pal_time_deadline_expired(now_ms, deadline_ms)) {
            fprintf(
                stderr, "timeout waiting for %s: observed=%d expected=%d\n",
                name, observed, expected);
            exit(1);
        }
        CHECK(h2_pal_time_sleep_ms(api->time, 1u) == H2_PAL_OK);
    }
}

static void wait_for_server_idle(
    fake_runtime_t *runtime,
    const h2_bleikcp_api_t *api,
    uint16_t conn_handle) {
    uint16_t rx_value_handle = 0u;
    CHECK(runtime->service != NULL);
    for (size_t i = 0u; i < runtime->service->characteristic_count; ++i) {
        if (runtime->service->characteristics[i].write != NULL) {
            rx_value_handle = (uint16_t)(2u + 2u * i);
            break;
        }
    }
    CHECK(rx_value_handle != 0u);

    uint64_t started_ms = 0u;
    CHECK(h2_pal_time_get_monotonic_ms(api->time, &started_ms) == H2_PAL_OK);
    uint64_t deadline_ms = h2_pal_time_deadline_ms(started_ms, TEST_IO_TIMEOUT_MS);
    int observed = H2_PAL_OK;
    for (;;) {
        observed = fake_gatt_write(
            runtime, conn_handle, rx_value_handle, NULL, 0u, false, 0u);
        if (observed == H2_PAL_ERR_WOULD_BLOCK) return;
        CHECK(observed == H2_PAL_ERR_INVALID_ARG || observed == H2_PAL_ERR_CLOSED);
        uint64_t now_ms = 0u;
        CHECK(h2_pal_time_get_monotonic_ms(api->time, &now_ms) == H2_PAL_OK);
        if (h2_pal_time_deadline_expired(now_ms, deadline_ms)) {
            fprintf(
                stderr,
                "timeout waiting for server idle: conn=%u status=%d\n",
                (unsigned)conn_handle, observed);
            exit(1);
        }
        CHECK(h2_pal_time_sleep_ms(api->time, 1u) == H2_PAL_OK);
    }
}

static void test_flush_result_precedence(const h2_bleikcp_api_t *api) {
    h2_bleikcp_resolved_config_t config;
    CHECK(h2_bleikcp_resolve_config(api, NULL, &config) == H2_PAL_OK);
    h2_bleikcp_t *stream = NULL;
    CHECK(h2_bleikcp_stream_create(
              api, &config, H2_BLEIKCP_ROLE_CLIENT, 10u, 244u, false,
              &stream) == H2_PAL_OK);

    stream->closing = true;
    stream->fatal_status = H2_PAL_ERR_IO;
    CHECK(h2_bleikcp_flush(stream, 0u) == H2_PAL_ERR_IO);

    stream->fatal_status = H2_PAL_OK;
    CHECK(h2_bleikcp_flush(stream, 0u) == H2_PAL_OK);

    stream->tx.len = 1u;
    CHECK(h2_bleikcp_flush(stream, 0u) == H2_PAL_ERR_CLOSED);
    stream->tx.len = 0u;
    stream->stats.waitsnd = 1u;
    CHECK(h2_bleikcp_flush(stream, 0u) == H2_PAL_ERR_CLOSED);
    stream->stats.waitsnd = 0u;

    CHECK(h2_bleikcp_stream_destroy(stream) == H2_PAL_OK);
}

static void test_nonprogress_does_not_wake_data_waiters(
    fake_runtime_t *runtime,
    const h2_bleikcp_api_t *api) {
    h2_bleikcp_resolved_config_t config;
    CHECK(h2_bleikcp_resolve_config(api, NULL, &config) == H2_PAL_OK);
    h2_bleikcp_t *stream = NULL;
    CHECK(h2_bleikcp_stream_create(
              api, &config, H2_BLEIKCP_ROLE_SERVER, 10u, 244u, false,
              &stream) == H2_PAL_OK);
    CHECK(stream->read_cond != stream->write_cond);

    const uint8_t input[] = {0x01u};
    int broadcasts_before = h2_atomic_load(&runtime->cond_broadcasts);
    CHECK(h2_bleikcp_stream_input(stream, input, sizeof(input)) == H2_PAL_OK);
    CHECK(stream->input.count == 1u);
    CHECK(h2_atomic_load(&runtime->cond_broadcasts) == broadcasts_before);

    const uint8_t output[] = {0x02u};
    CHECK(h2_bleikcp_write(stream, output, sizeof(output), 0u) == H2_PAL_OK);
    CHECK(stream->tx.len == 1u);
    CHECK(h2_atomic_load(&runtime->cond_broadcasts) == broadcasts_before);

    CHECK(h2_bleikcp_stream_destroy(stream) == H2_PAL_OK);

    stream = NULL;
    CHECK(h2_bleikcp_stream_create(
              api, &config, H2_BLEIKCP_ROLE_SERVER, 11u, 244u, false,
              &stream) == H2_PAL_OK);
    broadcasts_before = h2_atomic_load(&runtime->cond_broadcasts);
    CHECK(h2_bleikcp_stream_start(stream) == H2_PAL_OK);
    CHECK(h2_pal_time_sleep_ms(api->time, 50u) == H2_PAL_OK);
    CHECK(h2_atomic_load(&runtime->cond_broadcasts) == broadcasts_before);
    CHECK(h2_bleikcp_stream_destroy(stream) == H2_PAL_OK);
}

/* A full input frame queue drops the datagram for the peer's KCP to
 * retransmit; it must not close the stream or publish a fatal status. */
static void test_input_overflow_drops_frames(const h2_bleikcp_api_t *api) {
    const h2_bleikcp_config_t config = { .input_frame_capacity = 2u };
    h2_bleikcp_resolved_config_t resolved;
    CHECK(h2_bleikcp_resolve_config(api, &config, &resolved) == H2_PAL_OK);
    h2_bleikcp_t *stream = NULL;
    CHECK(h2_bleikcp_stream_create(
              api, &resolved, H2_BLEIKCP_ROLE_SERVER, 10u, 244u, false,
              &stream) == H2_PAL_OK);

    /* A KCP window-size segment (cmd 84) for the stream's conv: valid input
     * that only updates the remote window. */
    uint8_t frame[H2_BLEIKCP_KCP_OVERHEAD] = {0};
    frame[0] = (uint8_t)resolved.value.conv;
    frame[1] = (uint8_t)(resolved.value.conv >> 8u);
    frame[2] = (uint8_t)(resolved.value.conv >> 16u);
    frame[3] = (uint8_t)(resolved.value.conv >> 24u);
    frame[4] = 84u;
    frame[6] = 8u;

    CHECK(h2_bleikcp_stream_input(stream, frame, sizeof(frame)) == H2_PAL_OK);
    CHECK(h2_bleikcp_stream_input(stream, frame, sizeof(frame)) == H2_PAL_OK);
    CHECK(h2_bleikcp_stream_input(stream, frame, sizeof(frame)) ==
          H2_PAL_ERR_FULL);
    CHECK(stream->input.count == 2u);
    CHECK(!stream->closing);
    CHECK(stream->fatal_status == H2_PAL_OK);
    h2_bleikcp_stats_t stats;
    CHECK(h2_bleikcp_get_stats(stream, &stats) == H2_PAL_OK);
    CHECK(stats.dropped_input == 1u);
    CHECK(stats.rx_frames == 0u);

    /* The worker drains the queued frames and the stream keeps running. */
    CHECK(h2_bleikcp_stream_start(stream) == H2_PAL_OK);
    uint64_t started_ms = 0u;
    CHECK(h2_pal_time_get_monotonic_ms(api->time, &started_ms) == H2_PAL_OK);
    for (;;) {
        CHECK(h2_bleikcp_get_stats(stream, &stats) == H2_PAL_OK);
        if (stats.rx_frames == 2u) break;
        uint64_t now_ms = 0u;
        CHECK(h2_pal_time_get_monotonic_ms(api->time, &now_ms) == H2_PAL_OK);
        CHECK(now_ms - started_ms < TEST_IO_TIMEOUT_MS);
        CHECK(h2_pal_time_sleep_ms(api->time, 1u) == H2_PAL_OK);
    }
    CHECK(h2_bleikcp_stream_input(stream, frame, sizeof(frame)) == H2_PAL_OK);
    for (;;) {
        CHECK(h2_bleikcp_get_stats(stream, &stats) == H2_PAL_OK);
        if (stats.rx_frames == 3u) break;
        uint64_t now_ms = 0u;
        CHECK(h2_pal_time_get_monotonic_ms(api->time, &now_ms) == H2_PAL_OK);
        CHECK(now_ms - started_ms < TEST_IO_TIMEOUT_MS);
        CHECK(h2_pal_time_sleep_ms(api->time, 1u) == H2_PAL_OK);
    }
    (void)h2_pal_mutex_lock(api->sync, stream->mutex);
    CHECK(!stream->closing);
    CHECK(stream->fatal_status == H2_PAL_OK);
    (void)h2_pal_mutex_unlock(api->sync, stream->mutex);
    CHECK(stats.dropped_input == 1u);
    CHECK(stats.input_errors == 0u);
    CHECK(h2_bleikcp_stream_destroy(stream) == H2_PAL_OK);
}

static void gate_workers(fake_runtime_t *runtime, int parked) {
    pthread_mutex_lock(&runtime->gate_mutex);
    runtime->gate_armed = 1;
    while (runtime->gate_parked < parked) {
        pthread_cond_wait(&runtime->gate_cond, &runtime->gate_mutex);
    }
    pthread_mutex_unlock(&runtime->gate_mutex);
}

static void release_workers(fake_runtime_t *runtime) {
    pthread_mutex_lock(&runtime->gate_mutex);
    runtime->gate_armed = 0;
    pthread_cond_broadcast(&runtime->gate_cond);
    pthread_mutex_unlock(&runtime->gate_mutex);
}

typedef struct overflow_handler_state {
    const h2_bleikcp_api_t *api;
    h2_atomic_ptr_t stream;
    h2_atomic_int_t received;
    h2_atomic_int_t finish;
    h2_atomic_int_t done;
} overflow_handler_state_t;

/* Publishes the borrowed stream, reads the 300 bytes the client sends after
 * the overflow so the session provably still carries data, and keeps the
 * session open until the test has read the final stats. */
static int overflow_handler(void *user, h2_bleikcp_t *stream, uint16_t conn_handle) {
    overflow_handler_state_t *state = user;
    (void)conn_handle;
    h2_atomic_store(&state->stream, stream);
    uint8_t buffer[64];
    while (h2_atomic_load(&state->received) < 300) {
        size_t len = 0u;
        int rc = h2_bleikcp_read(stream, buffer, sizeof(buffer), &len, TEST_IO_TIMEOUT_MS);
        if (rc != H2_PAL_OK) return rc;
        for (size_t i = 0u; i < len; ++i) {
            if (buffer[i] != (uint8_t)((h2_atomic_load(&state->received) + i) & 0xffu)) {
                return H2_PAL_ERR_FORMAT;
            }
        }
        h2_atomic_fetch_add(&state->received, (int)len);
    }
    wait_for_atomic_at_least(state->api, &state->finish, 1, "overflow finish");
    h2_atomic_store(&state->done, 1);
    return H2_PAL_OK;
}

/* A GATT write that finds the active session's input queue full is served:
 * the server maps the drop to H2_PAL_OK so a synchronous provider never
 * feeds H2_PAL_ERR_FULL into the peer's KCP output, and both the server and
 * the client stream keep working once the peer retransmits. */
static void test_server_write_drops_on_full_queue(
    fake_runtime_t *runtime,
    const h2_bleikcp_api_t *api) {
    const h2_bleikcp_config_t config = { .input_frame_capacity = 2u };
    h2_bleikcp_resolved_config_t resolved;
    CHECK(h2_bleikcp_resolve_config(api, &config, &resolved) == H2_PAL_OK);
    overflow_handler_state_t state = { .api = api };
    CHECK(h2_atomic_ptr_init(&state.stream, NULL) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&state.received, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&state.finish, 0) == H2_ATOMIC_OK);
    CHECK(h2_atomic_int_init(&state.done, 0) == H2_ATOMIC_OK);
    h2_bleikcp_server_t *server = NULL;
    CHECK(h2_bleikcp_server_open(
              api, &config, overflow_handler, &state, &server) == H2_PAL_OK);
    const uint16_t conn_handle = 12u;
    const h2_pal_ble_connection_t connection = {
        .conn_handle = conn_handle,
        .role = H2_PAL_BLE_ROLE_PERIPHERAL,
        .mtu = 244u,
    };
    CHECK(fake_post_payload(
              runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
              &connection, sizeof(connection)) == H2_PAL_OK);
    h2_bleikcp_t *client = NULL;
    CHECK(h2_bleikcp_client_open(api, &config, conn_handle, 244u, &client) == H2_PAL_OK);
    uint64_t started_ms = 0u;
    CHECK(h2_pal_time_get_monotonic_ms(api->time, &started_ms) == H2_PAL_OK);
    while (h2_atomic_load(&state.stream) == NULL) {
        uint64_t now_ms = 0u;
        CHECK(h2_pal_time_get_monotonic_ms(api->time, &now_ms) == H2_PAL_OK);
        CHECK(now_ms - started_ms < TEST_IO_TIMEOUT_MS);
        CHECK(h2_pal_time_sleep_ms(api->time, 1u) == H2_PAL_OK);
    }
    h2_bleikcp_t *stream = h2_atomic_load(&state.stream);

    /* Park the server and client workers so nothing drains the queue. */
    gate_workers(runtime, 2);
    uint8_t frame[H2_BLEIKCP_KCP_OVERHEAD] = {0};
    frame[0] = (uint8_t)resolved.value.conv;
    frame[1] = (uint8_t)(resolved.value.conv >> 8u);
    frame[2] = (uint8_t)(resolved.value.conv >> 16u);
    frame[3] = (uint8_t)(resolved.value.conv >> 24u);
    frame[4] = 84u;
    frame[6] = 8u;
    (void)h2_pal_mutex_lock(api->sync, stream->mutex);
    size_t queued = stream->input.count;
    (void)h2_pal_mutex_unlock(api->sync, stream->mutex);
    CHECK(queued <= 2u);
    for (size_t i = queued; i < 3u; ++i) {
        CHECK(h2_pal_ble_gatt_write(
                  api->ble, conn_handle, stream->rx_value_handle, frame,
                  sizeof(frame), false, 1000u) == H2_PAL_OK);
    }
    h2_bleikcp_stats_t stats;
    CHECK(h2_bleikcp_get_stats(stream, &stats) == H2_PAL_OK);
    CHECK(stats.dropped_input == 1u);
    (void)h2_pal_mutex_lock(api->sync, stream->mutex);
    CHECK(stream->input.count == 2u);
    CHECK(!stream->closing);
    CHECK(stream->fatal_status == H2_PAL_OK);
    (void)h2_pal_mutex_unlock(api->sync, stream->mutex);
    (void)h2_pal_mutex_lock(api->sync, client->mutex);
    CHECK(!client->closing);
    CHECK(client->fatal_status == H2_PAL_OK);
    (void)h2_pal_mutex_unlock(api->sync, client->mutex);
    release_workers(runtime);

    /* The session still carries data after the drop. */
    uint8_t request[300];
    for (size_t i = 0u; i < sizeof(request); ++i) request[i] = (uint8_t)(i & 0xffu);
    CHECK(h2_bleikcp_write(client, request, sizeof(request), 1000u) == H2_PAL_OK);
    CHECK(h2_bleikcp_flush(client, TEST_IO_TIMEOUT_MS) == H2_PAL_OK);
    wait_for_atomic_at_least(api, &state.received, 300, "overflow bytes");
    /* The transfer itself overflows the two-frame queue again; every drop
     * was recovered by the client's retransmit. */
    CHECK(h2_bleikcp_get_stats(stream, &stats) == H2_PAL_OK);
    CHECK(stats.dropped_input >= 1u);
    CHECK(stats.input_errors == 0u);
    (void)h2_pal_mutex_lock(api->sync, stream->mutex);
    CHECK(!stream->closing);
    CHECK(stream->fatal_status == H2_PAL_OK);
    (void)h2_pal_mutex_unlock(api->sync, stream->mutex);
    h2_atomic_store(&state.finish, 1);
    wait_for_atomic_at_least(api, &state.done, 1, "overflow handler");
    CHECK(h2_bleikcp_close(client) == H2_PAL_OK);
    wait_for_server_idle(runtime, api, conn_handle);
    CHECK(h2_bleikcp_server_close(server) == H2_PAL_OK);
    CHECK(runtime->service == NULL);
    runtime->unregister_count = 0;
    h2_atomic_destroy(&state.stream);
    h2_atomic_destroy(&state.received);
    h2_atomic_destroy(&state.finish);
    h2_atomic_destroy(&state.done);
}

static void test_task_name_ownership(const h2_bleikcp_api_t *api) {
    const h2_bleikcp_config_t config = {
        .worker_task_options = { .name = "caller/worker", .min_stack_size = 7u * 1024u },
        .server_task_options = { .name = "caller/server", .min_stack_size = 8u * 1024u },
    };
    h2_bleikcp_resolved_config_t resolved;
    CHECK(h2_bleikcp_resolve_config(api, &config, &resolved) == H2_PAL_OK);
    CHECK(strcmp(
              resolved.value.worker_task_options.name,
              h2_bleikcp_worker_task_name) == 0);
    CHECK(strcmp(
              resolved.value.server_task_options.name,
              h2_bleikcp_server_task_name) == 0);
    CHECK(resolved.value.worker_task_options.min_stack_size == 7u * 1024u);
    CHECK(resolved.value.server_task_options.min_stack_size == 8u * 1024u);
}

static h2_atomic_int_t s_extra_writes;

static h2_pal_result_t extra_write(
    void *user,
    const h2_pal_ble_gatt_access_t *access,
    const uint8_t *data,
    size_t len) {
    (void)user;
    CHECK(access != NULL && access->attr_handle == 6u);
    CHECK(len == 3u && memcmp(data, "abc", 3u) == 0);
    h2_atomic_fetch_add(&s_extra_writes, 1);
    return H2_PAL_OK;
}

static int idle_handler(void *user, h2_bleikcp_t *stream, uint16_t conn_handle) {
    (void)user;
    (void)stream;
    (void)conn_handle;
    return H2_PAL_OK;
}

/* Caller characteristics join the server's own service after TX and RX. */
static void test_extra_characteristics(
    fake_runtime_t *runtime,
    const h2_bleikcp_api_t *api) {
    static const uint8_t extra_uuid[] = { 0xe3u, 0xfeu };
    uint16_t extra_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    uint16_t extra_cccd_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    const h2_pal_ble_gatt_characteristic_t extra = {
        .uuid = { extra_uuid, sizeof(extra_uuid) },
        .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP |
                      H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
        .permissions = H2_PAL_BLE_GATT_PERMISSION_WRITE,
        .write = extra_write,
        .out_value_handle = &extra_value_handle,
        .out_cccd_handle = &extra_cccd_handle,
    };
    h2_bleikcp_server_t *server = NULL;
    h2_bleikcp_config_t config = {
        .extra_characteristics = &extra,
        .extra_characteristic_count =
            H2_BLEIKCP_SERVER_EXTRA_CHARACTERISTIC_MAX + 1u,
    };
    CHECK(h2_bleikcp_server_open(api, &config, idle_handler, NULL, &server) ==
          H2_PAL_ERR_INVALID_ARG);
    config.extra_characteristics = NULL;
    config.extra_characteristic_count = 1u;
    CHECK(h2_bleikcp_server_open(api, &config, idle_handler, NULL, &server) ==
          H2_PAL_ERR_INVALID_ARG);
    CHECK(server == NULL);

    config.extra_characteristics = &extra;
    CHECK(h2_bleikcp_server_open(api, &config, idle_handler, NULL, &server) ==
          H2_PAL_OK);
    CHECK(runtime->service != NULL);
    CHECK(runtime->service->characteristic_count == 3u);
    CHECK(runtime->service->characteristics[2].write == extra_write);
    CHECK(extra_value_handle == 6u && extra_cccd_handle == 7u);
    CHECK(h2_pal_ble_gatt_write(
              api->ble, 5u, extra_value_handle, (const uint8_t *)"abc", 3u,
              false, 1000u) == H2_PAL_OK);
    CHECK(h2_atomic_load(&s_extra_writes) == 1);
    CHECK(h2_bleikcp_server_close(server) == H2_PAL_OK);
    CHECK(runtime->service == NULL);
    runtime->unregister_count = 0;
}

static void stream_event(
    void *user,
    h2_bleikcp_t *stream,
    h2_bleikcp_event_t event,
    uint16_t conn_handle,
    int status) {
    event_state_t *state = user;
    CHECK(conn_handle >= 7u && conn_handle <= 9u);
    if (stream != NULL) {
        h2_bleikcp_stats_t stats;
        CHECK(h2_bleikcp_get_stats(stream, &stats) == H2_PAL_OK);
        CHECK(stats.att_mtu == 244u);
        CHECK(stats.kcp_mtu == 241u);
    }
    if (event == H2_BLEIKCP_EVENT_CONNECTED) {
        CHECK(status == H2_PAL_OK);
        h2_atomic_fetch_add(&state->connected, 1);
    } else if (event == H2_BLEIKCP_EVENT_READY) {
        CHECK(stream != NULL);
        CHECK(status == H2_PAL_OK);
        h2_atomic_fetch_add(&state->ready, 1);
    } else if (event == H2_BLEIKCP_EVENT_DISCONNECTED) {
        CHECK(stream != NULL);
        CHECK(status == H2_PAL_ERR_CLOSED);
        if (stream->role == H2_BLEIKCP_ROLE_CLIENT) {
            h2_atomic_fetch_add(&state->client_disconnected, 1);
        }
    } else if (event == H2_BLEIKCP_EVENT_PROTOCOL_ERROR) {
        CHECK(stream == NULL);
        CHECK(status == H2_PAL_ERR_UNSUPPORTED);
        h2_atomic_fetch_add(&state->protocol_error, 1);
    }
}

static int server_handler(void *user, h2_bleikcp_t *stream, uint16_t conn_handle) {
    handler_state_t *state = user;
    int call = h2_atomic_fetch_add(&state->calls, 1) + 1;
    CHECK(conn_handle == (uint16_t)(6 + call));
    size_t transfer_len = call == 1 ? 1024u : (call == 2 ? 257u : 33u);
    uint8_t request[73];
    size_t received = 0u;
    while (received < transfer_len) {
        size_t request_len = 0u;
        int rc = h2_bleikcp_read(
            stream, request, sizeof(request), &request_len,
            TEST_IO_TIMEOUT_MS);
        if (rc != H2_PAL_OK) return rc;
        for (size_t i = 0u; i < request_len; ++i) {
            if (request[i] != (uint8_t)((received + i) & 0xffu)) return H2_PAL_ERR_FORMAT;
        }
        received += request_len;
    }
    uint8_t reply[1024];
    for (size_t i = 0u; i < transfer_len; ++i) reply[i] = (uint8_t)(255u - (i & 0xffu));
    int rc = h2_bleikcp_write(
        stream, reply, transfer_len, TEST_IO_TIMEOUT_MS);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_bleikcp_flush(stream, TEST_IO_TIMEOUT_MS);
    if (rc != H2_PAL_OK) return rc;
    wait_for_atomic_at_least(
        state->api, &state->replies_drained, call, "client reply drain");
    return H2_PAL_OK;
}

static void run_client_exchange(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_config_t *config,
    fake_runtime_t *runtime,
    handler_state_t *handler_state,
    const event_state_t *event_state,
    uint16_t conn_handle,
    size_t transfer_len,
    bool expect_retransmit) {
    h2_bleikcp_t *client = NULL;
    CHECK(h2_bleikcp_client_open(
              api, config, conn_handle, 244u, &client) == H2_PAL_OK);
    int session = (int)conn_handle - 6;
    wait_for_atomic_at_least(api, &handler_state->calls, session, "server handler");
    uint8_t request[1024];
    for (size_t i = 0u; i < transfer_len; ++i) request[i] = (uint8_t)(i & 0xffu);
    CHECK(h2_bleikcp_write(client, request, transfer_len, 1000u) == H2_PAL_OK);
    CHECK(h2_bleikcp_flush(client, TEST_IO_TIMEOUT_MS) == H2_PAL_OK);
    uint8_t reply[31];
    size_t received = 0u;
    while (received < transfer_len) {
        size_t reply_len = 0u;
        CHECK(h2_bleikcp_read(
                  client, reply, sizeof(reply), &reply_len,
                  TEST_IO_TIMEOUT_MS) == H2_PAL_OK);
        for (size_t i = 0u; i < reply_len; ++i) {
            CHECK(reply[i] == (uint8_t)(255u - ((received + i) & 0xffu)));
        }
        received += reply_len;
    }
    h2_atomic_fetch_add(&handler_state->replies_drained, 1);
    wait_for_atomic_at_least(
        api, &event_state->client_disconnected, session,
        "client disconnect event");
    CHECK(h2_bleikcp_flush(client, TEST_IO_TIMEOUT_MS) == H2_PAL_OK);
    h2_bleikcp_stats_t stats;
    CHECK(h2_bleikcp_get_stats(client, &stats) == H2_PAL_OK);
    if (expect_retransmit) CHECK(stats.retransmits > 0u);
    CHECK(h2_bleikcp_close(client) == H2_PAL_OK);
    wait_for_server_idle(runtime, api, conn_handle);
}

static void test_per_service_unregister(void) {
    const h2_pal_result_t results[] = {
        H2_PAL_OK, H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_BUSY, H2_PAL_ERR_NOT_FOUND,
    };
    for (size_t i = 0u; i < sizeof(results) / sizeof(results[0]); ++i) {
        fake_runtime_t runtime;
        fake_runtime_init(&runtime);
        h2_pal_ble_vtable_t vtable = *runtime.ble.vtable;
        vtable.unregister_gatt_service = fake_unregister_service;
        runtime.ble.vtable = &vtable;
        runtime.unregister_service_result = results[i];
        h2_bleikcp_api_t api = {
            .ble = &runtime.ble, .task = &runtime.task, .time = &runtime.time,
            .sync = &runtime.sync, .system_event = &runtime.events,
            .allocator = &runtime.allocator,
        };
        h2_bleikcp_config_t config = {0};
        handler_state_t handler_state = { .api = &api };
        handler_state_init(&handler_state);
        h2_bleikcp_server_t *service = NULL;
        CHECK(h2_bleikcp_server_open(
                  &api, &config, server_handler, &handler_state,
                  &service) == H2_PAL_OK);
        h2_pal_result_t expected = results[i] == H2_PAL_ERR_UNSUPPORTED
            ? H2_PAL_OK : results[i];
        CHECK(h2_bleikcp_server_close(service) == expected);
        CHECK(runtime.unregister_service_count == 1);
        CHECK(runtime.unregister_count == (results[i] == H2_PAL_ERR_UNSUPPORTED));
        if (expected != H2_PAL_OK) {
            CHECK(runtime.service != NULL);
            runtime.unregister_service_result = H2_PAL_OK;
            CHECK(h2_bleikcp_server_close(service) == H2_PAL_OK);
            CHECK(runtime.unregister_service_count == 2);
            CHECK(runtime.unregister_count == 0);
        }
        CHECK(runtime.service == NULL);
        handler_state_destroy(&handler_state);
        fake_runtime_atomics_destroy(&runtime);
    CHECK(pthread_mutex_destroy(&runtime.event_mutex) == 0);
    }
}

static int discard_kcp_output(const char *data, int len, ikcpcb *kcp, void *user) {
    (void)data; (void)len; (void)kcp; (void)user;
    return 0;
}

static void test_kcp_allocators(void) {
    /* A different KCP consumer can already own libc blocks when BLE installs
     * its hooks, and can continue to allocate after installation. */
    ikcpcb *legacy = ikcp_create(1u, NULL);
    CHECK(legacy != NULL);
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_test_allocator_t arenas[2];
    h2_bleikcp_t *streams[2];
    size_t calls[2];
    for (size_t i = 0; i < 2u; ++i) {
        h2_test_allocator_init(&arenas[i]);
        h2_bleikcp_api_t api = {
            .ble = &runtime.ble, .task = &runtime.task, .time = &runtime.time,
            .sync = &runtime.sync, .system_event = &runtime.events,
            .allocator = &arenas[i].api,
        };
        h2_bleikcp_config_t config = {.conv = (uint32_t)i + 10u};
        h2_bleikcp_resolved_config_t resolved;
        CHECK(h2_bleikcp_resolve_config(&api, &config, &resolved) == H2_PAL_OK);
        CHECK(h2_bleikcp_stream_create(&api, &resolved, H2_BLEIKCP_ROLE_CLIENT,
                  10u, 244u, false, &streams[i]) == H2_PAL_OK);
        ikcp_setoutput(streams[i]->kcp, discard_kcp_output);
        calls[i] = h2_atomic_load(&arenas[i].calls);
        uint8_t frame[25] = {0};
        frame[0] = (uint8_t)config.conv;
        frame[4] = 81u; /* PUSH creates RX segment and grows the ACK list. */
        frame[6] = 32u;
        frame[20] = 1u;
        frame[24] = 42u;
        CHECK(h2_bleikcp_stream_input(streams[i], frame, sizeof(frame)) == H2_PAL_OK);
        CHECK(h2_bleikcp_write(streams[i], frame + 24, 1u, 0u) == H2_PAL_OK);
        CHECK(h2_bleikcp_stream_start(streams[i]) == H2_PAL_OK);
    }
    for (size_t i = 0; i < 2u; ++i) {
        uint8_t byte;
        size_t len = 0u;
        CHECK(h2_bleikcp_read(streams[i], &byte, 1u, &len, TEST_IO_TIMEOUT_MS) == H2_PAL_OK);
        CHECK(len == 1u && byte == 42u);
        CHECK(h2_atomic_load(&arenas[i].calls) >= calls[i] + 3u);
        CHECK(h2_bleikcp_stream_destroy(streams[i]) == H2_PAL_OK);
        CHECK(h2_atomic_load(&arenas[i].live) == 0u);
        h2_test_allocator_destroy(&arenas[i]);
    }
    CHECK(ikcp_send(legacy, "x", 1) >= 0);
    ikcp_release(legacy);
    fake_runtime_atomics_destroy(&runtime);
    CHECK(pthread_mutex_destroy(&runtime.event_mutex) == 0);
    CHECK(pthread_mutex_destroy(&runtime.gate_mutex) == 0);
    CHECK(pthread_cond_destroy(&runtime.gate_cond) == 0);
}

int main(void) {
    CHECK(h2_atomic_int_init(&s_extra_writes, 0) == H2_ATOMIC_OK);
    test_kcp_allocators();
    test_per_service_unregister();
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_bleikcp_api_t api = {
        .ble = &runtime.ble,
        .task = &runtime.task,
        .time = &runtime.time,
        .sync = &runtime.sync,
        .system_event = &runtime.events,
        .allocator = &runtime.allocator,
    };
    test_task_name_ownership(&api);
    test_flush_result_precedence(&api);
    test_nonprogress_does_not_wake_data_waiters(&runtime, &api);
    test_input_overflow_drops_frames(&api);
    test_server_write_drops_on_full_queue(&runtime, &api);
    test_extra_characteristics(&runtime, &api);
    handler_state_t handler_state = { .api = &api };
    handler_state_init(&handler_state);
    event_state_t event_state = {0};
    event_state_init(&event_state);
    h2_bleikcp_config_t config = {
        .on_event = stream_event,
        .user = &event_state,
    };
    h2_pal_ble_gatt_service_t management_service = {0};
    runtime.service = &management_service;
    runtime.service_count = 1u;
    h2_atomic_store(&runtime.fail_next_register, 1);
    h2_bleikcp_server_t *failed_server = NULL;
    CHECK(h2_bleikcp_server_open(
              &api, &config, server_handler, &handler_state,
              &failed_server) == H2_PAL_ERR_NO_MEMORY);
    CHECK(failed_server == NULL);
    CHECK(runtime.service == &management_service);
    CHECK(runtime.service_count == 1u);
    CHECK(runtime.unregister_count == 0);

    h2_bleikcp_server_t *server = NULL;
    CHECK(h2_bleikcp_server_open(
              &api, &config, server_handler, &handler_state, &server) == H2_PAL_OK);
    CHECK(runtime.service != NULL);
    CHECK(runtime.service->characteristic_count == 2u);

    h2_bleikcp_config_t quiet_config = {0};
    runtime.rx_properties_override = H2_PAL_BLE_GATT_PROPERTY_WRITE;
    h2_bleikcp_t *write_with_response_only = NULL;
    CHECK(h2_bleikcp_client_open(
              &api, &quiet_config, 6u, 244u,
              &write_with_response_only) == H2_PAL_ERR_UNSUPPORTED);
    CHECK(write_with_response_only == NULL);
    runtime.rx_properties_override = 0u;

    h2_bleikcp_t *retry_close = NULL;
    CHECK(h2_bleikcp_client_open(
              &api, &quiet_config, 6u, 244u, &retry_close) == H2_PAL_OK);
    h2_atomic_store(&runtime.fail_next_join, 1);
    CHECK(h2_bleikcp_close(retry_close) == H2_PAL_ERR_TASK);
    CHECK(h2_bleikcp_close(retry_close) == H2_PAL_OK);

    h2_bleikcp_t *low_mtu = NULL;
    CHECK(h2_bleikcp_client_open(&api, &config, 7u, 23u, &low_mtu) == H2_PAL_ERR_UNSUPPORTED);
    CHECK(low_mtu == NULL);

    h2_pal_ble_connection_t connection = {
        .conn_handle = 7u,
        .role = H2_PAL_BLE_ROLE_PERIPHERAL,
        .mtu = 23u,
    };
    CHECK(fake_post_payload(
              &runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
              &connection, sizeof(connection)) == H2_PAL_OK);

    h2_pal_ble_gatt_subscribe_t low_mtu_subscribe = {
        .value_handle = 2u,
        .cccd_handle = 3u,
        .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
        .enable = true,
    };
    CHECK(h2_pal_ble_gatt_subscribe(
              &runtime.ble, 7u, &low_mtu_subscribe, 1000u) == H2_PAL_OK);
    h2_pal_ble_mtu_info_t mtu = { .conn_handle = 7u, .mtu = 244u };
    CHECK(fake_post_payload(
              &runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
              &mtu, sizeof(mtu)) == H2_PAL_OK);

    h2_atomic_store(&runtime.drop_next_gatt_write, 1);
    run_client_exchange(
        &api, &config, &runtime, &handler_state, &event_state,
        7u, 1024u, true);

    connection.conn_handle = 8u;
    connection.mtu = 244u;
    CHECK(fake_post_payload(
              &runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
              &connection, sizeof(connection)) == H2_PAL_OK);
    run_client_exchange(
        &api, &config, &runtime, &handler_state, &event_state,
        8u, 257u, false);

    connection.conn_handle = 9u;
    CHECK(fake_post_payload(
              &runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
              &connection, sizeof(connection)) == H2_PAL_OK);
    h2_pal_ble_subscription_state_t disabled = {
        .conn_handle = 9u,
        .value_handle = 2u,
        .mode = H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE,
        .enabled = false,
    };
    CHECK(fake_post_payload(
              &runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED,
              &disabled, sizeof(disabled)) == H2_PAL_OK);
    CHECK(h2_atomic_load(&handler_state.calls) == 2);
    run_client_exchange(
        &api, &config, &runtime, &handler_state, &event_state,
        9u, 33u, false);

    h2_atomic_store(&runtime.fail_next_join, 1);
    CHECK(h2_bleikcp_server_close(server) == H2_PAL_ERR_TASK);
    CHECK(runtime.service != NULL);
    CHECK(runtime.unregister_count == 0);
    CHECK(h2_bleikcp_server_close(server) == H2_PAL_OK);
    CHECK(h2_atomic_load(&handler_state.calls) == 3);
    CHECK(h2_atomic_load(&event_state.connected) == 6);
    CHECK(h2_atomic_load(&event_state.ready) == 6);
    CHECK(h2_atomic_load(&event_state.protocol_error) == 1);
    CHECK(runtime.unregister_count == 1);
    CHECK(runtime.subscription_count == 0u);
    handler_state_destroy(&handler_state);
    event_state_destroy(&event_state);
    h2_atomic_int_destroy(&s_extra_writes);
    fake_runtime_atomics_destroy(&runtime);
    CHECK(pthread_mutex_destroy(&runtime.event_mutex) == 0);
    puts("bleikcp tests passed");
    return 0;
}
