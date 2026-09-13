#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

struct h2_jieli_sdk_mutex { pthread_mutex_t lock; };
static atomic_int live, parked, resume_lock;
static _Thread_local int park_next;

h2_jieli_sdk_mutex_t *h2_jieli_sdk_mutex_create(void) {
    h2_jieli_sdk_mutex_t *m = malloc(sizeof(*m));
    assert(m && pthread_mutex_init(&m->lock, NULL) == 0);
    atomic_fetch_add(&live, 1);
    return m;
}
void h2_jieli_sdk_mutex_destroy(h2_jieli_sdk_mutex_t *m) {
    assert(pthread_mutex_destroy(&m->lock) == 0);
    free(m);
    atomic_fetch_sub(&live, 1);
}
int h2_jieli_sdk_mutex_lock(h2_jieli_sdk_mutex_t *m, uint32_t timeout) {
    (void)timeout;
    if (park_next) {
        park_next = 0;
        atomic_store(&parked, 1);
        while (!atomic_load(&resume_lock)) sched_yield();
    }
    return pthread_mutex_lock(&m->lock);
}
int h2_jieli_sdk_mutex_unlock(h2_jieli_sdk_mutex_t *m) {
    return pthread_mutex_unlock(&m->lock);
}
static int handler(void *user, const h2_pal_system_event_t *event) {
    (void)user; (void)event; return H2_PAL_OK;
}
static void *operation(void *arg) {
    const h2_pal_system_event_api_t *api = h2_jieli_wl82_platform_system_event_api();
    park_next = 1;
    if (arg != NULL) {
        h2_pal_system_event_subscription_t *sub;
        assert(h2_pal_system_event_subscribe(api,
            H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, handler, NULL, &sub) == H2_PAL_OK);
    } else {
        const h2_pal_system_event_t event = {
            .type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
        };
        assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
    }
    return NULL;
}
int main(void) {
    const h2_pal_system_event_api_t *api = h2_jieli_wl82_platform_system_event_api();
    for (unsigned round = 0; round < 100; ++round) {
        assert(h2_pal_system_event_init(api) == H2_PAL_OK);
        atomic_store(&parked, 0); atomic_store(&resume_lock, 0);
        pthread_t thread;
        assert(pthread_create(&thread, NULL, operation, round % 2 ? NULL : &thread) == 0);
        while (!atomic_load(&parked)) sched_yield();
        h2_pal_system_event_deinit(api);
        assert(atomic_load(&live) == 1); /* retained before first mutex lock */
        assert(h2_pal_system_event_init(api) == H2_PAL_ERR_BUSY);
        atomic_store(&resume_lock, 1);
        assert(pthread_join(thread, NULL) == 0);
        assert(atomic_load(&live) == 0);
        assert(h2_pal_system_event_init(api) == H2_PAL_OK);
        h2_pal_system_event_deinit(api);
        assert(atomic_load(&live) == 0);
    }
    return 0;
}
