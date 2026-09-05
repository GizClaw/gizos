#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_ble_wifi_config_internal.h"

#include "h2_runtime.h"

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

#define TEST_CONN_HANDLE ((uint16_t)7u)
#define TEST_ATT_MTU ((uint16_t)247u)
#define TEST_WAIT_MS 5000u
#define TEST_MAX_NOTIFICATIONS 32u
#define TEST_MAX_SCAN_ENTRIES 8u

struct h2_pal_mutex { pthread_mutex_t value; };
struct h2_pal_cond { pthread_cond_t value; };
struct h2_pal_task { pthread_t thread; h2_pal_task_entry_t entry; void *ctx; };
struct h2_pal_system_event_subscription {
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *user;
};

typedef struct fake_notification {
    uint16_t attr_handle;
    uint8_t data[H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN];
    size_t len;
} fake_notification_t;

typedef struct fake_runtime {
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    h2_pal_system_event_subscription_t *subscriptions[16];
    size_t subscription_count;

    const h2_pal_ble_gatt_service_t *service;
    int unregister_count;
    int fail_next_unregister;
    int adv_start_count;
    int adv_stop_count;
    int adv_data_count;

    fake_notification_t notifications[TEST_MAX_NOTIFICATIONS];
    size_t notification_count;
    int notify_result;
    bool hold_notify;
    bool notify_active;
    bool posted_during_notify;
    bool reconnect_done;
    int notifies_after_reconnect;
    h2_ble_wifi_config_t *config_service;
    uint32_t generation_during_send;
    bool send_observed;

    h2_pal_wifi_scan_entry_t scan_entries[TEST_MAX_SCAN_ENTRIES];
    size_t scan_entry_count;
    int scan_result;
    int scan_calls;
    int filtered_scan_calls;
    bool hold_scan;
    bool scan_in_progress;

    int connect_result;
    int connect_calls;
    bool hold_connect;
    bool connect_active;
    h2_pal_wifi_sta_status_t status;
    char connected_ssid[H2_PAL_WIFI_SSID_MAX + 1];
    char connected_password[H2_PAL_WIFI_PASSWORD_MAX + 1];

    int events[64];
    size_t event_count;

    h2_pal_mem_api_t allocator;
    h2_pal_ble_t ble;
    h2_pal_wifi_sta_t wifi_sta;
    h2_pal_task_api_t task;
    h2_pal_sync_api_t sync;
    h2_pal_system_event_api_t system_event;
} fake_runtime_t;

/* Handles the fake GATT server assigns, mirroring the schema order. */
#define TEST_COMMAND_HANDLE ((uint16_t)2u)
#define TEST_SCAN_HANDLE ((uint16_t)4u)
#define TEST_PROVISION_HANDLE ((uint16_t)6u)

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
    if (mutex == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (pthread_mutex_init(&mutex->value, NULL) != 0) {
        h2_pal_mem_free(config->allocator, mutex);
        return H2_PAL_ERR_IO;
    }
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    fake_runtime_t *runtime = user;
    (void)pthread_mutex_destroy(&mutex->value);
    h2_pal_mem_free(&runtime->allocator, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    return pthread_mutex_lock(&mutex->value) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t fake_mutex_try_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    int rc = pthread_mutex_trylock(&mutex->value);
    return rc == 0 ? H2_PAL_OK
                   : (rc == EBUSY ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO);
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
    if (cond == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (pthread_cond_init(&cond->value, NULL) != 0) {
        h2_pal_mem_free(config->allocator, cond);
        return H2_PAL_ERR_IO;
    }
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_cond_destroy(void *user, h2_pal_cond_t *cond) {
    fake_runtime_t *runtime = user;
    (void)pthread_cond_destroy(&cond->value);
    h2_pal_mem_free(&runtime->allocator, cond);
    return H2_PAL_OK;
}

static void fake_deadline(struct timespec *out, uint32_t timeout_ms) {
    (void)clock_gettime(CLOCK_REALTIME, out);
    out->tv_sec += timeout_ms / 1000u;
    out->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec++;
        out->tv_nsec -= 1000000000L;
    }
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
    fake_deadline(&deadline, timeout_ms);
    int rc = pthread_cond_timedwait(&cond->value, &mutex->value, &deadline);
    return rc == 0 ? H2_PAL_OK
                   : (rc == ETIMEDOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO);
}

static h2_pal_result_t fake_cond_signal(void *user, h2_pal_cond_t *cond) {
    (void)user;
    return pthread_cond_signal(&cond->value) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t fake_cond_broadcast(void *user, h2_pal_cond_t *cond) {
    (void)user;
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
    CHECK(options != NULL && options->name != NULL);
    h2_pal_task_t *task = h2_pal_mem_alloc(&runtime->allocator, sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
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
    int rc = pthread_join(task->thread, NULL) == 0 ? H2_PAL_OK : H2_PAL_ERR_TASK;
    if (rc == H2_PAL_OK) {
        h2_pal_mem_free(&runtime->allocator, task);
    }
    return rc;
}

static const h2_pal_task_vtable_t s_task_vtable = {
    .start = fake_task_start,
    .join = fake_task_join,
};

static int fake_event_subscribe(
    void *user,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    fake_runtime_t *runtime = user;
    h2_pal_system_event_subscription_t *subscription = malloc(sizeof(*subscription));
    if (subscription == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    *subscription = (h2_pal_system_event_subscription_t){
        .type = type,
        .handler = handler,
        .user = handler_user,
    };
    pthread_mutex_lock(&runtime->mutex);
    CHECK(runtime->subscription_count < 16u);
    runtime->subscriptions[runtime->subscription_count++] = subscription;
    pthread_mutex_unlock(&runtime->mutex);
    *out_subscription = subscription;
    return H2_PAL_OK;
}

static void fake_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription) {
    fake_runtime_t *runtime = user;
    if (subscription == NULL) {
        return;
    }
    pthread_mutex_lock(&runtime->mutex);
    for (size_t i = 0u; i < runtime->subscription_count; ++i) {
        if (runtime->subscriptions[i] == subscription) {
            runtime->subscriptions[i] =
                runtime->subscriptions[--runtime->subscription_count];
            break;
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
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
    pthread_mutex_lock(&runtime->mutex);
    for (size_t i = 0u; i < runtime->subscription_count; ++i) {
        if (runtime->subscriptions[i]->type == event->type) {
            snapshot[count++] = runtime->subscriptions[i];
        }
    }
    pthread_mutex_unlock(&runtime->mutex);
    for (size_t i = 0u; i < count; ++i) {
        (void)snapshot[i]->handler(snapshot[i]->user, event);
    }
    return H2_PAL_OK;
}

static const h2_pal_system_event_vtable_t s_event_vtable = {
    .subscribe = fake_event_subscribe,
    .unsubscribe = fake_event_unsubscribe,
    .post = fake_event_post,
};

static h2_pal_result_t fake_register(
    void *user,
    const h2_pal_ble_gatt_service_t *services,
    size_t count) {
    fake_runtime_t *runtime = user;
    if (count != 1u || services == NULL ||
        services[0].characteristic_count != H2_BLE_WIFI_CONFIG_CHARACTERISTIC_COUNT) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    pthread_mutex_lock(&runtime->mutex);
    runtime->service = services;
    pthread_mutex_unlock(&runtime->mutex);
    if (services[0].out_service_handle != NULL) {
        *services[0].out_service_handle = 1u;
    }
    for (size_t i = 0u; i < services[0].characteristic_count; ++i) {
        const h2_pal_ble_gatt_characteristic_t *ch = &services[0].characteristics[i];
        if (ch->out_value_handle != NULL) {
            *ch->out_value_handle = (uint16_t)(2u + 2u * i);
        }
        if (ch->out_cccd_handle != NULL) {
            *ch->out_cccd_handle = (uint16_t)(3u + 2u * i);
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t fake_unregister(void *user) {
    fake_runtime_t *runtime = user;
    pthread_mutex_lock(&runtime->mutex);
    runtime->unregister_count++;
    if (runtime->fail_next_unregister != 0) {
        runtime->fail_next_unregister = 0;
        pthread_mutex_unlock(&runtime->mutex);
        return H2_PAL_ERR_BUSY;
    }
    runtime->service = NULL;
    pthread_mutex_unlock(&runtime->mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_notify(
    void *user,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len) {
    fake_runtime_t *runtime = user;
    CHECK(conn_handle == TEST_CONN_HANDLE);
    CHECK(len <= H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN);
    pthread_mutex_lock(&runtime->mutex);
    /* Count on entry: a send already in flight is not a post-reconnect send. */
    if (runtime->reconnect_done) {
        runtime->notifies_after_reconnect++;
    }
    bool hold = runtime->hold_notify;
    if (hold) {
        /*
         * Stay inside the send while another task tries to change the
         * connection state, so the test can observe whether the service
         * serializes the two.
         */
        runtime->hold_notify = false;
        runtime->notify_active = true;
        pthread_cond_broadcast(&runtime->cond);
        pthread_mutex_unlock(&runtime->mutex);
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 200000000L };
        (void)nanosleep(&delay, NULL);
        /*
         * Still inside the Host call: the connection identity this send was
         * validated against must not have moved on.
         */
        h2_ble_wifi_config_t *config_service = runtime->config_service;
        CHECK(h2_pal_mutex_lock(&runtime->sync, config_service->mutex) == H2_PAL_OK);
        uint32_t generation = config_service->conn_generation;
        (void)h2_pal_mutex_unlock(&runtime->sync, config_service->mutex);
        pthread_mutex_lock(&runtime->mutex);
        runtime->generation_during_send = generation;
        runtime->send_observed = true;
        runtime->notify_active = false;
        pthread_cond_broadcast(&runtime->cond);
    }
    int result = runtime->notify_result;
    if (result == H2_PAL_OK && runtime->notification_count < TEST_MAX_NOTIFICATIONS) {
        fake_notification_t *entry = &runtime->notifications[runtime->notification_count++];
        entry->attr_handle = attr_handle;
        entry->len = len;
        memcpy(entry->data, data, len);
    }
    pthread_cond_broadcast(&runtime->cond);
    pthread_mutex_unlock(&runtime->mutex);
    return (h2_pal_result_t)result;
}

static h2_pal_result_t fake_set_adv_data(void *user, const h2_pal_ble_adv_data_t *data) {
    fake_runtime_t *runtime = user;
    (void)data;
    pthread_mutex_lock(&runtime->mutex);
    runtime->adv_data_count++;
    pthread_mutex_unlock(&runtime->mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_start_advertising(
    void *user,
    const h2_pal_ble_adv_params_t *params) {
    fake_runtime_t *runtime = user;
    (void)params;
    pthread_mutex_lock(&runtime->mutex);
    runtime->adv_start_count++;
    pthread_cond_broadcast(&runtime->cond);
    pthread_mutex_unlock(&runtime->mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_stop_advertising(void *user) {
    fake_runtime_t *runtime = user;
    pthread_mutex_lock(&runtime->mutex);
    runtime->adv_stop_count++;
    pthread_cond_broadcast(&runtime->cond);
    pthread_mutex_unlock(&runtime->mutex);
    return H2_PAL_OK;
}

static const h2_pal_ble_vtable_t s_ble_vtable = {
    .set_adv_data = fake_set_adv_data,
    .start_advertising = fake_start_advertising,
    .stop_advertising = fake_stop_advertising,
    .register_gatt_services = fake_register,
    .unregister_gatt_services = fake_unregister,
    .notify = fake_notify,
};

static int fake_wifi_scan(
    void *user,
    const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result,
    void *callback_user,
    uint32_t timeout_ms) {
    fake_runtime_t *runtime = user;
    (void)timeout_ms;
    h2_pal_wifi_scan_entry_t entries[TEST_MAX_SCAN_ENTRIES];
    size_t count;
    int result;
    pthread_mutex_lock(&runtime->mutex);
    runtime->scan_calls++;
    if (request != NULL && request->ssid_len > 0u) {
        runtime->filtered_scan_calls++;
    }
    runtime->scan_in_progress = true;
    pthread_cond_broadcast(&runtime->cond);
    while (runtime->hold_scan) {
        pthread_cond_wait(&runtime->cond, &runtime->mutex);
    }
    runtime->scan_in_progress = false;
    count = runtime->scan_entry_count;
    memcpy(entries, runtime->scan_entries, sizeof(entries));
    result = runtime->scan_result;
    pthread_mutex_unlock(&runtime->mutex);

    if (result != H2_PAL_OK) {
        return result;
    }
    /*
     * A filtered request is a hint to the radio, not a promise that the
     * callback only sees matches, so deliver every entry and let the caller
     * do its own matching.
     */
    (void)request;
    for (size_t i = 0u; i < count; ++i) {
        /* Both Wi-Fi providers keep scanning while the callback returns true. */
        if (!on_result(callback_user, &entries[i])) {
            break;
        }
    }
    return H2_PAL_OK;
}

static int fake_wifi_connect(
    void *user,
    const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms) {
    fake_runtime_t *runtime = user;
    (void)timeout_ms;
    pthread_mutex_lock(&runtime->mutex);
    runtime->connect_calls++;
    if (runtime->hold_connect) {
        /* Stay inside the attempt so the test can queue progress and
         * replace the peer while the worker is busy. */
        runtime->connect_active = true;
        pthread_cond_broadcast(&runtime->cond);
        while (runtime->hold_connect) {
            pthread_cond_wait(&runtime->cond, &runtime->mutex);
        }
        runtime->connect_active = false;
    }
    memcpy(runtime->connected_ssid, config->ssid, config->ssid_len);
    runtime->connected_ssid[config->ssid_len] = '\0';
    memcpy(runtime->connected_password, config->password, config->password_len);
    runtime->connected_password[config->password_len] = '\0';
    int result = runtime->connect_result;
    pthread_cond_broadcast(&runtime->cond);
    pthread_mutex_unlock(&runtime->mutex);
    return result;
}

static int fake_wifi_get_status(void *user, h2_pal_wifi_sta_status_t *out_status) {
    fake_runtime_t *runtime = user;
    pthread_mutex_lock(&runtime->mutex);
    *out_status = runtime->status;
    pthread_mutex_unlock(&runtime->mutex);
    return H2_PAL_OK;
}

static const h2_pal_wifi_sta_vtable_t s_wifi_vtable = {
    .get_status = fake_wifi_get_status,
    .scan = fake_wifi_scan,
    .connect = fake_wifi_connect,
};

static void fake_runtime_init(fake_runtime_t *runtime) {
    memset(runtime, 0, sizeof(*runtime));
    CHECK(pthread_mutex_init(&runtime->mutex, NULL) == 0);
    CHECK(pthread_cond_init(&runtime->cond, NULL) == 0);
    runtime->allocator = (h2_pal_mem_api_t){
        .user = runtime,
        .vtable = &s_allocator_vtable,
    };
    runtime->ble = (h2_pal_ble_t){
        .user = runtime,
        .vtable = &s_ble_vtable,
        .allocator = &runtime->allocator,
    };
    runtime->wifi_sta = (h2_pal_wifi_sta_t){
        .user = runtime,
        .vtable = &s_wifi_vtable,
    };
    runtime->task = (h2_pal_task_api_t){
        .user = runtime,
        .vtable = &s_task_vtable,
    };
    runtime->sync = (h2_pal_sync_api_t){
        .user = runtime,
        .vtable = &s_sync_vtable,
    };
    runtime->system_event = (h2_pal_system_event_api_t){
        .user = runtime,
        .vtable = &s_event_vtable,
    };
}

/*
 * The service reads station transitions from the Runtime's published
 * snapshot, so a test that exercises the address wait needs a real Runtime.
 * Every capability it does not touch is the canonical unsupported one.
 */
static void fake_runtime_deinit(fake_runtime_t *runtime) {
    (void)pthread_cond_destroy(&runtime->cond);
    (void)pthread_mutex_destroy(&runtime->mutex);
}

static void fake_api(fake_runtime_t *runtime, h2_ble_wifi_config_api_t *out_api) {
    *out_api = (h2_ble_wifi_config_api_t){
        .ble = &runtime->ble,
        .wifi_sta = &runtime->wifi_sta,
        .task = &runtime->task,
        .sync = &runtime->sync,
        .system_event = &runtime->system_event,
        .allocator = &runtime->allocator,
    };
}

static void fake_add_scan_entry(
    fake_runtime_t *runtime,
    const char *ssid,
    int rssi,
    h2_pal_wifi_security_t security) {
    CHECK(runtime->scan_entry_count < TEST_MAX_SCAN_ENTRIES);
    h2_pal_wifi_scan_entry_t *entry = &runtime->scan_entries[runtime->scan_entry_count++];
    memset(entry, 0, sizeof(*entry));
    size_t len = strlen(ssid);
    memcpy(entry->ssid, ssid, len);
    entry->ssid_len = len;
    entry->rssi = rssi;
    entry->security = security;
}

static void fake_post(
    fake_runtime_t *runtime,
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size) {
    h2_pal_system_event_t event = {
        .type = type,
        .payload = payload,
        .payload_size = payload_size,
    };
    (void)fake_event_post(runtime, &event, 0u);
}

static void fake_connect_peer(fake_runtime_t *runtime, uint16_t mtu) {
    h2_pal_ble_connection_t connection = {
        .conn_handle = TEST_CONN_HANDLE,
        .role = H2_PAL_BLE_ROLE_PERIPHERAL,
        .mtu = mtu,
    };
    fake_post(runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &connection,
              sizeof(connection));
}

static void fake_subscribe(fake_runtime_t *runtime, uint16_t value_handle, bool enable) {
    h2_pal_ble_subscription_state_t state = {
        .conn_handle = TEST_CONN_HANDLE,
        .value_handle = value_handle,
        .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
        .enabled = enable,
    };
    fake_post(runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED, &state,
              sizeof(state));
}

static h2_pal_result_t fake_gatt_write_at(
    fake_runtime_t *runtime,
    uint16_t attr_handle,
    uint16_t conn_handle,
    uint16_t offset,
    const uint8_t *data,
    size_t len) {
    pthread_mutex_lock(&runtime->mutex);
    const h2_pal_ble_gatt_service_t *service = runtime->service;
    pthread_mutex_unlock(&runtime->mutex);
    CHECK(service != NULL);
    for (size_t i = 0u; i < service->characteristic_count; ++i) {
        const h2_pal_ble_gatt_characteristic_t *ch = &service->characteristics[i];
        if ((uint16_t)(2u + 2u * i) != attr_handle) {
            continue;
        }
        CHECK(ch->write != NULL);
        h2_pal_ble_gatt_access_t access = {
            .conn_handle = conn_handle,
            .attr_handle = attr_handle,
            .offset = offset,
        };
        return ch->write(ch->user, &access, data, len);
    }
    CHECK(false);
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t fake_gatt_write_from(
    fake_runtime_t *runtime,
    uint16_t attr_handle,
    uint16_t conn_handle,
    const uint8_t *data,
    size_t len) {
    return fake_gatt_write_at(runtime, attr_handle, conn_handle, 0u, data, len);
}

static h2_pal_result_t fake_gatt_write(
    fake_runtime_t *runtime,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len) {
    return fake_gatt_write_from(runtime, attr_handle, TEST_CONN_HANDLE, data, len);
}

static void fake_wait_notifications(fake_runtime_t *runtime, size_t expected) {
    struct timespec deadline;
    fake_deadline(&deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime->mutex);
    while (runtime->notification_count < expected) {
        int rc = pthread_cond_timedwait(&runtime->cond, &runtime->mutex, &deadline);
        CHECK(rc == 0);
    }
    pthread_mutex_unlock(&runtime->mutex);
}

static void fake_wait_scan_in_progress(fake_runtime_t *runtime) {
    struct timespec deadline;
    fake_deadline(&deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime->mutex);
    while (!runtime->scan_in_progress) {
        int rc = pthread_cond_timedwait(&runtime->cond, &runtime->mutex, &deadline);
        CHECK(rc == 0);
    }
    pthread_mutex_unlock(&runtime->mutex);
}

static void fake_release_scan(fake_runtime_t *runtime) {
    pthread_mutex_lock(&runtime->mutex);
    runtime->hold_scan = false;
    pthread_cond_broadcast(&runtime->cond);
    pthread_mutex_unlock(&runtime->mutex);
}

static size_t fake_notification_count(fake_runtime_t *runtime) {
    pthread_mutex_lock(&runtime->mutex);
    size_t count = runtime->notification_count;
    pthread_mutex_unlock(&runtime->mutex);
    return count;
}

static fake_notification_t fake_notification(fake_runtime_t *runtime, size_t index) {
    pthread_mutex_lock(&runtime->mutex);
    CHECK(index < runtime->notification_count);
    fake_notification_t entry = runtime->notifications[index];
    pthread_mutex_unlock(&runtime->mutex);
    return entry;
}

static void test_on_event(
    void *user,
    h2_ble_wifi_config_t *service,
    h2_ble_wifi_config_event_t event,
    uint16_t conn_handle,
    int status) {
    fake_runtime_t *runtime = user;
    (void)service;
    (void)conn_handle;
    (void)status;
    pthread_mutex_lock(&runtime->mutex);
    if (runtime->event_count < 64u) {
        runtime->events[runtime->event_count++] = (int)event;
    }
    pthread_cond_broadcast(&runtime->cond);
    pthread_mutex_unlock(&runtime->mutex);
}

/* The caller must hold the runtime mutex. */
static bool fake_saw_event_locked(
    fake_runtime_t *runtime,
    h2_ble_wifi_config_event_t event) {
    for (size_t i = 0u; i < runtime->event_count; ++i) {
        if (runtime->events[i] == (int)event) {
            return true;
        }
    }
    return false;
}

/* Events are emitted on the worker after the notification that precedes them. */
static void fake_wait_event(fake_runtime_t *runtime, h2_ble_wifi_config_event_t event) {
    struct timespec deadline;
    fake_deadline(&deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime->mutex);
    while (!fake_saw_event_locked(runtime, event)) {
        CHECK(pthread_cond_timedwait(&runtime->cond, &runtime->mutex, &deadline) == 0);
    }
    pthread_mutex_unlock(&runtime->mutex);
}

static bool fake_saw_event(fake_runtime_t *runtime, h2_ble_wifi_config_event_t event) {
    pthread_mutex_lock(&runtime->mutex);
    bool found = fake_saw_event_locked(runtime, event);
    pthread_mutex_unlock(&runtime->mutex);
    return found;
}

static h2_ble_wifi_config_t *open_service(
    fake_runtime_t *runtime,
    const h2_ble_wifi_config_config_t *overrides) {
    h2_ble_wifi_config_api_t api;
    fake_api(runtime, &api);
    h2_ble_wifi_config_config_t config;
    if (overrides != NULL) {
        config = *overrides;
    } else {
        memset(&config, 0, sizeof(config));
    }
    config.on_event = test_on_event;
    config.user = runtime;
    h2_ble_wifi_config_t *service = NULL;
    CHECK(h2_ble_wifi_config_open(&api, &config, &service) == H2_PAL_OK);
    CHECK(service != NULL);
    return service;
}

static void connect_and_subscribe(fake_runtime_t *runtime) {
    fake_connect_peer(runtime, TEST_ATT_MTU);
    fake_subscribe(runtime, TEST_SCAN_HANDLE, true);
    fake_subscribe(runtime, TEST_PROVISION_HANDLE, true);
}

static void test_open_registers_schema(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);

    const h2_pal_ble_gatt_service_t *schema = h2_ble_wifi_config_gatt_service(service);
    CHECK(schema != NULL);
    CHECK(schema->primary);
    CHECK(schema->characteristic_count == H2_BLE_WIFI_CONFIG_CHARACTERISTIC_COUNT);
    CHECK(schema->uuid.len == 16u);
    CHECK(memcmp(schema->uuid.data, h2_ble_wifi_config_default_service_uuid, 16u) == 0);
    CHECK(memcmp(schema->characteristics[0].uuid.data,
                 h2_ble_wifi_config_default_command_uuid, 16u) == 0);
    CHECK(schema->characteristics[0].properties == H2_PAL_BLE_GATT_PROPERTY_WRITE);
    CHECK(memcmp(schema->characteristics[1].uuid.data,
                 h2_ble_wifi_config_default_scan_uuid, 16u) == 0);
    CHECK(schema->characteristics[1].properties == H2_PAL_BLE_GATT_PROPERTY_NOTIFY);
    CHECK(memcmp(schema->characteristics[2].uuid.data,
                 h2_ble_wifi_config_default_provision_uuid, 16u) == 0);
    CHECK(schema->characteristics[2].properties ==
          (H2_PAL_BLE_GATT_PROPERTY_WRITE | H2_PAL_BLE_GATT_PROPERTY_NOTIFY));

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    CHECK(runtime.unregister_count == 1);
    CHECK(runtime.subscription_count == 0u);
    fake_runtime_deinit(&runtime);
}

static void test_caller_owned_registration(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_config_t config;
    memset(&config, 0, sizeof(config));
    config.gatt_service_registered_by_caller = true;
    h2_ble_wifi_config_t *service = open_service(&runtime, &config);
    CHECK(runtime.service == NULL);
    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    CHECK(runtime.unregister_count == 0);
    fake_runtime_deinit(&runtime);
}

static void test_scan_reports_one_ap_per_notification(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    fake_add_scan_entry(&runtime, "", -60, H2_PAL_WIFI_SECURITY_OPEN);
    fake_add_scan_entry(&runtime, "cafe", -80, H2_PAL_WIFI_SECURITY_OPEN);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_OK);
    /* Two access points and one scan-finished frame; the hidden one is dropped. */
    fake_wait_notifications(&runtime, 3u);

    fake_notification_t first = fake_notification(&runtime, 0u);
    CHECK(first.attr_handle == TEST_SCAN_HANDLE);
    CHECK(first.len == 4u + 6u);
    CHECK(first.data[0] == 0x01u);
    CHECK(first.data[1] == 0xd3u);
    CHECK(first.data[2] == H2_BLE_WIFI_CONFIG_AP_FLAG_SECURED);
    CHECK(first.data[3] == 6u);
    CHECK(memcmp(first.data + 4, "office", 6) == 0);

    fake_notification_t second = fake_notification(&runtime, 1u);
    CHECK(second.len == 4u + 4u);
    CHECK(second.data[2] == 0u);
    CHECK(memcmp(second.data + 4, "cafe", 4) == 0);

    fake_notification_t finished = fake_notification(&runtime, 2u);
    CHECK(finished.attr_handle == TEST_SCAN_HANDLE);
    CHECK(finished.len == 1u);
    CHECK(finished.data[0] == 0x02u);

    h2_ble_wifi_config_stats_t stats;
    CHECK(h2_ble_wifi_config_get_stats(service, &stats) == H2_PAL_OK);
    CHECK(stats.scans_started == 1u);
    CHECK(stats.aps_reported == 2u);
    CHECK(stats.aps_dropped == 1u);
    CHECK(stats.att_mtu == TEST_ATT_MTU);
    fake_wait_event(&runtime, H2_BLE_WIFI_CONFIG_EVENT_SCAN_FINISHED);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_scan_error_frame(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    runtime.scan_result = H2_PAL_ERR_IO;
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_OK);
    fake_wait_notifications(&runtime, 1u);
    fake_notification_t frame = fake_notification(&runtime, 0u);
    CHECK(frame.len == 1u);
    CHECK(frame.data[0] == 0x03u);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_repeated_scan_start_is_idempotent(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    runtime.hold_scan = true;
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_OK);
    fake_wait_scan_in_progress(&runtime);
    /* A second start while the first scan runs must not queue another scan. */
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_OK);
    fake_release_scan(&runtime);
    fake_wait_notifications(&runtime, 2u);

    h2_ble_wifi_config_stats_t stats;
    CHECK(h2_ble_wifi_config_get_stats(service, &stats) == H2_PAL_OK);
    CHECK(stats.scans_started == 1u);
    CHECK(runtime.scan_calls == 1);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_scan_requires_subscription(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    fake_connect_peer(&runtime, TEST_ATT_MTU);

    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_ERR_INVALID_STATE);
    CHECK(runtime.scan_calls == 0);

    const uint8_t bad[] = { 0x09u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, bad, sizeof(bad)) ==
          H2_PAL_ERR_FORMAT);
    const uint8_t too_long[] = { 0x01u, 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, too_long, sizeof(too_long)) ==
          H2_PAL_ERR_FORMAT);

    h2_ble_wifi_config_stats_t stats;
    CHECK(h2_ble_wifi_config_get_stats(service, &stats) == H2_PAL_OK);
    CHECK(stats.protocol_errors == 3u);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void write_credentials(
    fake_runtime_t *runtime,
    const char *ssid,
    const char *password) {
    uint8_t frame[H2_BLE_WIFI_CONFIG_CREDENTIALS_FRAME_MAX_LEN];
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    frame[0] = (uint8_t)ssid_len;
    memcpy(frame + 1, ssid, ssid_len);
    frame[1u + ssid_len] = (uint8_t)password_len;
    memcpy(frame + 2 + ssid_len, password, password_len);
    CHECK(fake_gatt_write(runtime, TEST_PROVISION_HANDLE, frame,
                          2u + ssid_len + password_len) == H2_PAL_OK);
}

static void test_provision_success(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    /* The requested network is not the first entry the radio reports. */
    fake_add_scan_entry(&runtime, "neighbour", -70, H2_PAL_WIFI_SECURITY_WPA2);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    runtime.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
    runtime.status.ip_valid = 1u;
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    write_credentials(&runtime, "office", "hunter2!");
    fake_wait_notifications(&runtime, 1u);
    fake_notification_t result = fake_notification(&runtime, 0u);
    CHECK(result.attr_handle == TEST_PROVISION_HANDLE);
    CHECK(result.len == 3u);
    CHECK(result.data[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(result.data[1] == 0x00u);
    CHECK(result.data[2] == 0x00u);
    CHECK(runtime.connect_calls == 1);
    CHECK(strcmp(runtime.connected_ssid, "office") == 0);
    CHECK(strcmp(runtime.connected_password, "hunter2!") == 0);
    /* The credential path verifies the access point with a filtered scan. */
    CHECK(runtime.filtered_scan_calls == 1);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_provision_wrong_password(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    runtime.connect_result = H2_PAL_ERR_TIMEOUT;
    runtime.status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    runtime.status.disconnect_reason = 15; /* 4-way handshake timeout. */
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    write_credentials(&runtime, "office", "wrong");
    fake_wait_notifications(&runtime, 1u);
    fake_notification_t result = fake_notification(&runtime, 0u);
    CHECK(result.data[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(result.data[1] == 0x01u);
    CHECK(result.data[2] == (uint8_t)H2_BLE_WIFI_CONFIG_REASON_BAD_PASSWORD);
    fake_wait_event(&runtime, H2_BLE_WIFI_CONFIG_EVENT_PROVISION_FAILED);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_provision_ap_not_found(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    write_credentials(&runtime, "elsewhere", "hunter2!");
    fake_wait_notifications(&runtime, 1u);
    fake_notification_t result = fake_notification(&runtime, 0u);
    CHECK(result.data[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(result.data[1] == 0x01u);
    CHECK(result.data[2] == (uint8_t)H2_BLE_WIFI_CONFIG_REASON_AP_NOT_FOUND);
    /* A missing access point is reported without a connect attempt. */
    CHECK(runtime.connect_calls == 0);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_provision_dhcp_failure(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    runtime.status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
    runtime.status.ip_valid = 0u;
    /* The address never lands, so the wait must give up on its own budget. */
    h2_ble_wifi_config_config_t config;
    memset(&config, 0, sizeof(config));
    config.dhcp_timeout_ms = 200u;
    h2_ble_wifi_config_t *service = open_service(&runtime, &config);
    connect_and_subscribe(&runtime);

    write_credentials(&runtime, "office", "hunter2!");
    fake_wait_notifications(&runtime, 1u);
    fake_notification_t result = fake_notification(&runtime, 0u);
    CHECK(result.data[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(result.data[1] == 0x01u);
    CHECK(result.data[2] == (uint8_t)H2_BLE_WIFI_CONFIG_REASON_DHCP_FAILED);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_provision_open_network(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "guest", -55, H2_PAL_WIFI_SECURITY_OPEN);
    runtime.status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
    runtime.status.ip_valid = 1u;
    h2_ble_wifi_config_config_t config;
    memset(&config, 0, sizeof(config));
    config.skip_ap_verification_before_connect = true;
    h2_ble_wifi_config_t *service = open_service(&runtime, &config);
    connect_and_subscribe(&runtime);

    write_credentials(&runtime, "guest", "");
    fake_wait_notifications(&runtime, 1u);
    fake_notification_t result = fake_notification(&runtime, 0u);
    CHECK(result.data[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(result.data[1] == 0x00u);
    CHECK(runtime.connect_calls == 1);
    CHECK(runtime.connected_password[0] == '\0');
    /* Verification was skipped, so no scan ran at all. */
    CHECK(runtime.scan_calls == 0);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_malformed_credentials_report_failure(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    /* Declares a 32-byte SSID but carries two bytes. */
    const uint8_t truncated[] = { 32u, 'a', 'b' };
    CHECK(fake_gatt_write(&runtime, TEST_PROVISION_HANDLE, truncated,
                          sizeof(truncated)) == H2_PAL_ERR_FORMAT);
    fake_wait_notifications(&runtime, 1u);
    fake_notification_t result = fake_notification(&runtime, 0u);
    CHECK(result.attr_handle == TEST_PROVISION_HANDLE);
    CHECK(result.data[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(result.data[1] == 0x01u);
    CHECK(result.data[2] == (uint8_t)H2_BLE_WIFI_CONFIG_REASON_UNKNOWN);
    CHECK(runtime.connect_calls == 0);
    fake_wait_event(&runtime, H2_BLE_WIFI_CONFIG_EVENT_PROTOCOL_ERROR);

    /* An empty write must not crash or start a connect attempt either. */
    const uint8_t empty[] = { 0u };
    CHECK(fake_gatt_write(&runtime, TEST_PROVISION_HANDLE, empty, 0u) ==
          H2_PAL_ERR_FORMAT);
    CHECK(runtime.connect_calls == 0);

    /*
     * A long write cannot carry credentials either, and it owes the
     * application the same result frame: otherwise the app waits forever.
     */
    const uint8_t valid[] = { 1u, 'a', 0u };
    CHECK(fake_gatt_write_at(&runtime, TEST_PROVISION_HANDLE, TEST_CONN_HANDLE,
                             1u, valid, sizeof(valid)) ==
          H2_PAL_ERR_UNSUPPORTED);
    fake_wait_notifications(&runtime, 2u);
    fake_notification_t long_write_result = fake_notification(&runtime, 1u);
    CHECK(long_write_result.attr_handle == TEST_PROVISION_HANDLE);
    CHECK(long_write_result.len == 3u);
    CHECK(long_write_result.data[0] == H2_BLE_WIFI_CONFIG_PROVISION_FRAME_FINAL);
    CHECK(long_write_result.data[1] == 0x01u);
    CHECK(long_write_result.data[2] == (uint8_t)H2_BLE_WIFI_CONFIG_REASON_UNKNOWN);
    CHECK(runtime.connect_calls == 0);

    h2_ble_wifi_config_stats_t stats;
    CHECK(h2_ble_wifi_config_get_stats(service, &stats) == H2_PAL_OK);
    CHECK(stats.protocol_errors == 3u);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_advertising_pauses_during_wifi(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);

    h2_pal_ble_adv_params_t params = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 200u,
    };
    h2_pal_ble_adv_data_t data = { .local_name = "provision" };
    CHECK(h2_ble_wifi_config_start_advertising(service, &data, &params) == H2_PAL_OK);
    CHECK(runtime.adv_data_count == 1);
    CHECK(runtime.adv_start_count == 1);

    connect_and_subscribe(&runtime);
    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_OK);
    fake_wait_notifications(&runtime, 2u);
    CHECK(runtime.adv_stop_count == 1);
    CHECK(runtime.adv_start_count == 2);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    CHECK(runtime.adv_stop_count == 2);
    fake_runtime_deinit(&runtime);
}

static void test_advertising_kept_during_wifi(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    h2_ble_wifi_config_config_t config;
    memset(&config, 0, sizeof(config));
    config.keep_advertising_during_wifi = true;
    h2_ble_wifi_config_t *service = open_service(&runtime, &config);

    h2_pal_ble_adv_params_t params = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 200u,
    };
    CHECK(h2_ble_wifi_config_start_advertising(service, NULL, &params) == H2_PAL_OK);
    CHECK(runtime.adv_data_count == 0);

    connect_and_subscribe(&runtime);
    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_OK);
    fake_wait_notifications(&runtime, 2u);
    CHECK(runtime.adv_stop_count == 0);
    CHECK(runtime.adv_start_count == 1);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_small_mtu_reports_event(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    fake_connect_peer(&runtime, 23u);

    struct timespec deadline;
    fake_deadline(&deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime.mutex);
    while (runtime.event_count < 2u) {
        CHECK(pthread_cond_timedwait(&runtime.cond, &runtime.mutex, &deadline) == 0);
    }
    pthread_mutex_unlock(&runtime.mutex);
    CHECK(fake_saw_event(&runtime, H2_BLE_WIFI_CONFIG_EVENT_CONNECTED));
    CHECK(fake_saw_event(&runtime, H2_BLE_WIFI_CONFIG_EVENT_MTU_TOO_SMALL));

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_disconnect_clears_connection(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    h2_pal_ble_disconnected_info_t info = {
        .conn_handle = TEST_CONN_HANDLE,
        .reason = 19,
    };
    fake_post(&runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &info,
              sizeof(info));

    /* Writes from a stale connection handle are refused. */
    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_ERR_INVALID_STATE);
    CHECK(runtime.scan_calls == 0);
    CHECK(fake_notification_count(&runtime) == 0u);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_open_rejects_bad_arguments(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_api_t api;
    fake_api(&runtime, &api);
    h2_ble_wifi_config_t *service = (h2_ble_wifi_config_t *)0x1;

    CHECK(h2_ble_wifi_config_open(&api, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    h2_ble_wifi_config_api_t missing = api;
    missing.wifi_sta = NULL;
    CHECK(h2_ble_wifi_config_open(&missing, NULL, &service) == H2_PAL_ERR_INVALID_ARG);
    CHECK(service == NULL);

    h2_ble_wifi_config_config_t config;
    memset(&config, 0, sizeof(config));
    config.min_att_mtu = 64u;
    CHECK(h2_ble_wifi_config_open(&api, &config, &service) == H2_PAL_ERR_INVALID_ARG);

    memset(&config, 0, sizeof(config));
    static const uint8_t bad_uuid[3] = { 0u, 1u, 2u };
    config.service_uuid = (h2_pal_ble_uuid_t){ .data = bad_uuid, .len = 3u };
    CHECK(h2_ble_wifi_config_open(&api, &config, &service) == H2_PAL_ERR_INVALID_ARG);

    CHECK(h2_ble_wifi_config_close(NULL) == H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_ble_wifi_config_get_stats(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    CHECK(h2_ble_wifi_config_gatt_service(NULL) == NULL);
    fake_runtime_deinit(&runtime);
}

static void test_failed_unregister_keeps_the_service(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);

    /*
     * The Host still borrows the schema, so a failed release must not free
     * the instance; close() stays retryable.
     */
    runtime.fail_next_unregister = 1;
    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_ERR_BUSY);
    CHECK(runtime.unregister_count == 1);
    CHECK(runtime.service != NULL);
    CHECK(h2_ble_wifi_config_gatt_service(service) != NULL);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    CHECK(runtime.unregister_count == 2);
    CHECK(runtime.service == NULL);
    CHECK(runtime.subscription_count == 0u);
    fake_runtime_deinit(&runtime);
}

static void test_scan_frames_stay_with_their_connection(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    runtime.hold_scan = true;
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_OK);
    fake_wait_scan_in_progress(&runtime);

    /* The peer that started the scan leaves and the same handle is reused. */
    h2_pal_ble_disconnected_info_t info = {
        .conn_handle = TEST_CONN_HANDLE,
        .reason = 19,
    };
    fake_post(&runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &info,
              sizeof(info));
    connect_and_subscribe(&runtime);
    fake_release_scan(&runtime);

    /* Wait for the scan to finish without requiring any notification. */
    struct timespec deadline;
    fake_deadline(&deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime.mutex);
    while (!fake_saw_event_locked(&runtime, H2_BLE_WIFI_CONFIG_EVENT_SCAN_FINISHED)) {
        CHECK(pthread_cond_timedwait(&runtime.cond, &runtime.mutex, &deadline) == 0);
    }
    pthread_mutex_unlock(&runtime.mutex);
    /* The new peer must not inherit the previous peer's access points. */
    CHECK(fake_notification_count(&runtime) == 0u);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

static void test_rejection_stays_with_its_connection(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    /* A malformed write from a stale connection handle reaches nobody. */
    const uint8_t truncated[] = { 32u, 'a', 'b' };
    CHECK(fake_gatt_write_from(&runtime, TEST_PROVISION_HANDLE,
                               (uint16_t)(TEST_CONN_HANDLE + 1u), truncated,
                               sizeof(truncated)) == H2_PAL_ERR_FORMAT);
    struct timespec deadline;
    fake_deadline(&deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime.mutex);
    while (!fake_saw_event_locked(&runtime, H2_BLE_WIFI_CONFIG_EVENT_PROTOCOL_ERROR)) {
        CHECK(pthread_cond_timedwait(&runtime.cond, &runtime.mutex, &deadline) == 0);
    }
    pthread_mutex_unlock(&runtime.mutex);
    CHECK(fake_notification_count(&runtime) == 0u);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

typedef struct reconnect_thread_args {
    fake_runtime_t *runtime;
} reconnect_thread_args_t;

static void *reconnect_thread_main(void *ctx) {
    reconnect_thread_args_t *args = ctx;
    fake_runtime_t *runtime = args->runtime;

    struct timespec deadline;
    fake_deadline(&deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime->mutex);
    while (!runtime->notify_active) {
        CHECK(pthread_cond_timedwait(&runtime->cond, &runtime->mutex, &deadline) == 0);
    }
    pthread_mutex_unlock(&runtime->mutex);

    /* Reuse the same numeric handle for the peer that replaces the first. */
    h2_pal_ble_disconnected_info_t info = {
        .conn_handle = TEST_CONN_HANDLE,
        .reason = 19,
    };
    fake_post(runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &info,
              sizeof(info));
    fake_connect_peer(runtime, TEST_ATT_MTU);

    pthread_mutex_lock(&runtime->mutex);
    /*
     * The handler must return while the send is still in flight: blocking it
     * for the length of a Host call is exactly the reentrant deadlock the
     * deferral avoids.
     */
    runtime->posted_during_notify = runtime->notify_active;
    runtime->reconnect_done = true;
    pthread_mutex_unlock(&runtime->mutex);
    return NULL;
}

static void test_reconnect_cannot_interleave_with_a_send(void) {
    fake_runtime_t runtime;
    fake_runtime_init(&runtime);
    fake_add_scan_entry(&runtime, "office", -45, H2_PAL_WIFI_SECURITY_WPA2);
    fake_add_scan_entry(&runtime, "cafe", -80, H2_PAL_WIFI_SECURITY_OPEN);
    runtime.hold_notify = true;
    h2_ble_wifi_config_t *service = open_service(&runtime, NULL);
    connect_and_subscribe(&runtime);

    runtime.config_service = service;
    uint32_t generation_before = service->conn_generation;
    reconnect_thread_args_t args = { .runtime = &runtime };
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, reconnect_thread_main, &args) == 0);

    const uint8_t start[] = { 0x01u };
    CHECK(fake_gatt_write(&runtime, TEST_COMMAND_HANDLE, start, sizeof(start)) ==
          H2_PAL_OK);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(runtime.posted_during_notify);

    struct timespec send_deadline;
    fake_deadline(&send_deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime.mutex);
    while (!runtime.send_observed) {
        CHECK(pthread_cond_timedwait(&runtime.cond, &runtime.mutex, &send_deadline) == 0);
    }
    CHECK(runtime.generation_during_send == generation_before);
    pthread_mutex_unlock(&runtime.mutex);

    /*
     * The service cannot unsend a frame the Host already accepted, so it
     * reports the peer change and stops rather than sending more frames.
     */
    h2_ble_wifi_config_stats_t stats;
    CHECK(h2_ble_wifi_config_get_stats(service, &stats) == H2_PAL_OK);
    CHECK(stats.sends_during_peer_change == 1u);

    /* Nothing further from the first peer's scan reaches its replacement. */
    struct timespec deadline;
    fake_deadline(&deadline, TEST_WAIT_MS);
    pthread_mutex_lock(&runtime.mutex);
    while (!fake_saw_event_locked(&runtime, H2_BLE_WIFI_CONFIG_EVENT_SCAN_FINISHED)) {
        CHECK(pthread_cond_timedwait(&runtime.cond, &runtime.mutex, &deadline) == 0);
    }
    CHECK(runtime.notifies_after_reconnect == 0);
    pthread_mutex_unlock(&runtime.mutex);

    CHECK(h2_ble_wifi_config_close(service) == H2_PAL_OK);
    fake_runtime_deinit(&runtime);
}

int main(void) {
    test_open_registers_schema();
    test_caller_owned_registration();
    test_scan_reports_one_ap_per_notification();
    test_scan_error_frame();
    test_repeated_scan_start_is_idempotent();
    test_scan_requires_subscription();
    test_provision_success();
    test_provision_wrong_password();
    test_provision_ap_not_found();
    test_provision_dhcp_failure();
    test_provision_open_network();
    test_malformed_credentials_report_failure();
    test_advertising_pauses_during_wifi();
    test_advertising_kept_during_wifi();
    test_small_mtu_reports_event();
    test_disconnect_clears_connection();
    test_open_rejects_bad_arguments();
    test_failed_unregister_keeps_the_service();
    test_scan_frames_stay_with_their_connection();
    test_rejection_stays_with_its_connection();
    test_reconnect_cannot_interleave_with_a_send();
    printf("ok\n");
    return 0;
}
