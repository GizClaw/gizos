/*
 * The provisioning window needs a phone, so this test covers what the app
 * decides on its own: the capability gate, and that a window nobody uses
 * still releases the GATT schema and the advertising it started.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_smoke_ble_wifi_config.h"

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

struct h2_pal_mutex { pthread_mutex_t value; };
struct h2_pal_cond { pthread_cond_t value; };
struct h2_pal_task { pthread_t thread; h2_pal_task_entry_t entry; void *ctx; };
struct h2_pal_system_event_subscription { int used; };

typedef struct fake_host {
    int register_calls;
    int unregister_calls;
    int adv_start_calls;
    int adv_stop_calls;
    h2_pal_mem_api_t allocator;
    h2_pal_ble_t ble;
    h2_pal_wifi_sta_t wifi_sta;
    h2_pal_task_api_t task;
    h2_pal_sync_api_t sync;
    h2_pal_time_api_t time;
    h2_pal_system_event_api_t system_event;
    h2_runtime_t runtime;
} fake_host_t;

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
    CHECK(pthread_mutex_init(&mutex->value, NULL) == 0);
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    fake_host_t *host = user;
    (void)pthread_mutex_destroy(&mutex->value);
    h2_pal_mem_free(&host->allocator, mutex);
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
    CHECK(pthread_cond_init(&cond->value, NULL) == 0);
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_cond_destroy(void *user, h2_pal_cond_t *cond) {
    fake_host_t *host = user;
    (void)pthread_cond_destroy(&cond->value);
    h2_pal_mem_free(&host->allocator, cond);
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
    fake_host_t *host = user;
    (void)options;
    h2_pal_task_t *task = h2_pal_mem_alloc(&host->allocator, sizeof(*task));
    if (task == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    task->entry = entry;
    task->ctx = ctx;
    if (pthread_create(&task->thread, NULL, fake_task_main, task) != 0) {
        h2_pal_mem_free(&host->allocator, task);
        return H2_PAL_ERR_TASK;
    }
    *out_task = task;
    return H2_PAL_OK;
}

static int fake_task_join(void *user, h2_pal_task_t *task) {
    fake_host_t *host = user;
    int rc = pthread_join(task->thread, NULL) == 0 ? H2_PAL_OK : H2_PAL_ERR_TASK;
    if (rc == H2_PAL_OK) {
        h2_pal_mem_free(&host->allocator, task);
    }
    return rc;
}

static const h2_pal_task_vtable_t s_task_vtable = {
    .start = fake_task_start,
    .join = fake_task_join,
};

static h2_pal_result_t fake_monotonic(void *user, uint64_t *out_ms) {
    (void)user;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return H2_PAL_ERR_IO;
    }
    *out_ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return H2_PAL_OK;
}

/* The window is polled in real time; skip the wait instead of sleeping. */
static h2_pal_result_t fake_sleep(void *user, uint32_t ms) {
    (void)user;
    (void)ms;
    return H2_PAL_OK;
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
    (void)user;
    (void)type;
    (void)handler;
    (void)handler_user;
    h2_pal_system_event_subscription_t *subscription = malloc(sizeof(*subscription));
    if (subscription == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    subscription->used = 1;
    *out_subscription = subscription;
    return H2_PAL_OK;
}

static void fake_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription) {
    (void)user;
    free(subscription);
}

static const h2_pal_system_event_vtable_t s_event_vtable = {
    .subscribe = fake_event_subscribe,
    .unsubscribe = fake_event_unsubscribe,
};

static h2_pal_result_t fake_register(
    void *user,
    const h2_pal_ble_gatt_service_t *services,
    size_t count) {
    fake_host_t *host = user;
    CHECK(count == 1u);
    CHECK(services != NULL);
    host->register_calls++;
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
    fake_host_t *host = user;
    host->unregister_calls++;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_set_adv_data(
    void *user,
    const h2_pal_ble_adv_data_t *data) {
    (void)user;
    /* The applications filter on the service UUID, so it must be advertised. */
    CHECK(data != NULL);
    CHECK(data->local_name != NULL);
    CHECK(data->service_uuid_count == 1u);
    CHECK(data->service_uuids[0].len == 16u);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_start_advertising(
    void *user,
    const h2_pal_ble_adv_params_t *params) {
    fake_host_t *host = user;
    CHECK(params->mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE);
    host->adv_start_calls++;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_stop_advertising(void *user) {
    fake_host_t *host = user;
    host->adv_stop_calls++;
    return H2_PAL_OK;
}

static const h2_pal_ble_vtable_t s_ble_vtable = {
    .set_adv_data = fake_set_adv_data,
    .start_advertising = fake_start_advertising,
    .stop_advertising = fake_stop_advertising,
    .register_gatt_services = fake_register,
    .unregister_gatt_services = fake_unregister,
};

static int fake_wifi_get_status(void *user, h2_pal_wifi_sta_status_t *out_status) {
    (void)user;
    memset(out_status, 0, sizeof(*out_status));
    return H2_PAL_OK;
}

static const h2_pal_wifi_sta_vtable_t s_wifi_vtable = {
    .get_status = fake_wifi_get_status,
};

static void fake_host_init(fake_host_t *host) {
    memset(host, 0, sizeof(*host));
    host->allocator = (h2_pal_mem_api_t){
        .user = host,
        .vtable = &s_allocator_vtable,
    };
    host->ble = (h2_pal_ble_t){
        .user = host,
        .vtable = &s_ble_vtable,
        .allocator = &host->allocator,
    };
    host->wifi_sta = (h2_pal_wifi_sta_t){ .user = host, .vtable = &s_wifi_vtable };
    host->task = (h2_pal_task_api_t){ .user = host, .vtable = &s_task_vtable };
    host->sync = (h2_pal_sync_api_t){ .user = host, .vtable = &s_sync_vtable };
    host->time = (h2_pal_time_api_t){ .user = host, .vtable = &s_time_vtable };
    host->system_event = (h2_pal_system_event_api_t){
        .user = host,
        .vtable = &s_event_vtable,
    };
    host->runtime.ble_host = &host->ble;
    host->runtime.wifi_sta = &host->wifi_sta;
    host->runtime.task = &host->task;
    host->runtime.sync = &host->sync;
    host->runtime.time = &host->time;
    host->runtime.system_event = &host->system_event;
    host->runtime.mem = &host->allocator;
}

static void test_requires_capabilities(void) {
    CHECK(h2_smoke_ble_wifi_config_run(NULL) == H2_PAL_ERR_INVALID_ARG);

    fake_host_t host;
    fake_host_init(&host);
    host.runtime.wifi_sta = NULL;
    CHECK(h2_smoke_ble_wifi_config_run(&host.runtime) == H2_PAL_ERR_UNSUPPORTED);
    CHECK(host.register_calls == 0);

    fake_host_init(&host);
    host.runtime.ble_host = NULL;
    CHECK(h2_smoke_ble_wifi_config_run(&host.runtime) == H2_PAL_ERR_UNSUPPORTED);
    CHECK(host.register_calls == 0);
}

static void test_unused_window_closes(void) {
    fake_host_t host;
    fake_host_init(&host);
    /* No phone connects, so the window expires and must clean up after itself. */
    CHECK(h2_smoke_ble_wifi_config_run(&host.runtime) == H2_PAL_ERR_TIMEOUT);
    CHECK(host.register_calls == 1);
    CHECK(host.unregister_calls == 1);
    CHECK(host.adv_start_calls == 1);
    CHECK(host.adv_stop_calls == 1);
}

int main(void) {
    test_requires_capabilities();
    test_unused_window_closes();
    printf("ok\n");
    return 0;
}
