#include "h2_darwin_platform.h"
#include "h2_pal_core_e2e.h"
#include "host_config.h"
#include "host_observer.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Deliver through the actual Darwin provider on independent native threads.
 * post returns before the callback, as it does on ESP's event loop. */
static pthread_t deliveries[32];
static unsigned delivery_count;
static pthread_mutex_t delivery_lock = PTHREAD_MUTEX_INITIALIZER;
static const h2_pal_system_event_api_t *underlying;
typedef struct delivery {
    h2_pal_system_event_t event;
    unsigned char payload[];
} delivery_t;
static void *deliver(void *user) {
    delivery_t *work = user;
    assert(h2_pal_system_event_post(underlying, &work->event, 3000u) == H2_PAL_OK);
    free(work);
    return NULL;
}
static int initialize(void *user) {
    (void)user;
    return h2_pal_system_event_init(underlying);
}
static void deinitialize(void *user) {
    (void)user;
    for (unsigned i = 0; i < delivery_count; ++i)
        assert(pthread_join(deliveries[i], NULL) == 0);
    delivery_count = 0;
    h2_pal_system_event_deinit(underlying);
}
static int post(void *user, const h2_pal_system_event_t *event, uint32_t timeout) {
    (void)user;
    (void)timeout;
    int rc = h2_pal_system_event_validate(event);
    if (rc != H2_PAL_OK) return rc;
    delivery_t *work = malloc(sizeof(*work) + event->payload_size);
    if (work == NULL) return H2_PAL_ERR_NO_MEMORY;
    work->event = *event;
    if (event->payload_size) {
        memcpy(work->payload, event->payload, event->payload_size);
        work->event.payload = work->payload;
    }
    pthread_mutex_lock(&delivery_lock);
    assert(delivery_count < sizeof(deliveries) / sizeof(deliveries[0]));
    rc = pthread_create(&deliveries[delivery_count], NULL, deliver, work);
    if (rc == 0) ++delivery_count;
    pthread_mutex_unlock(&delivery_lock);
    if (rc != 0) { free(work); return H2_PAL_ERR_TASK; }
    return H2_PAL_OK;
}
static int subscribe(void *user, h2_pal_system_event_type_t type,
                     h2_pal_system_event_handler_t handler, void *context,
                     h2_pal_system_event_subscription_t **out) {
    (void)user;
    return h2_pal_system_event_subscribe(underlying, type, handler, context, out);
}
static void unsubscribe(void *user, h2_pal_system_event_subscription_t *subscription) {
    (void)user;
    h2_pal_system_event_unsubscribe(underlying, subscription);
}
static h2_pal_result_t clock_us(void *user, uint64_t *out) {
    (void)user;
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    *out = (uint64_t)now.tv_sec * 1000000u + now.tv_nsec / 1000u;
    return H2_PAL_OK;
}
int main(void) {
    alarm(30);
    underlying = h2_darwin_system_event_api();
    const h2_pal_system_event_vtable_t methods = {
        .init = initialize, .deinit = deinitialize, .post = post,
        .subscribe = subscribe, .unsubscribe = unsubscribe,
    };
    const h2_pal_system_event_api_t fixture = {.vtable = &methods};
    assert(h2_pal_core_host_observer_start() == H2_PAL_OK);
    h2_runtime_config_t native = h2_pal_core_host_config();
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&native, &runtime) == H2_PAL_OK);
    const h2_pal_core_e2e_config_t config = {
        .timeout_ms = 2000u, .queue_latest = H2_PAL_CORE_QUEUE_LATEST_REPLACE,
        .observe_monotonic_us = clock_us, .event_fixture = &fixture,
    };
    h2_pal_core_e2e_result_t result;
    (void)h2_pal_core_e2e_run(runtime, &config, &result);
    assert(result.cleanup_result == H2_PAL_OK);
    assert(result.retained_cleanup == NULL && result.baseline.retained_cleanup == NULL);
    assert(result.cases[15].status == H2_PAL_CORE_E2E_PASS);
    assert(result.cases[36].status == H2_PAL_CORE_E2E_PASS);
    assert(result.cases[37].status == H2_PAL_CORE_E2E_PASS);
    h2_runtime_deinit(runtime);
    assert(h2_pal_core_host_observer_stop() == H2_PAL_OK);
    return 0;
}
