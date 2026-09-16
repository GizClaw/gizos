#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

struct h2_jieli_sdk_mutex { pthread_mutex_t lock; };
static atomic_int live, parked, resume_lock, park_create, create_parked, resume_create;
static _Thread_local int park_next;

h2_jieli_sdk_mutex_t *h2_jieli_sdk_mutex_create(void) {
    if (atomic_load(&park_create)) {
        atomic_store(&create_parked, 1);
        while (!atomic_load(&resume_create)) sched_yield();
    }
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
static const h2_pal_system_event_t event = {
    .type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
};
static atomic_int calls;
static int owner_handler(void *user, const h2_pal_system_event_t *value) {
    (void)user; (void)value;
    atomic_fetch_add(&calls, 1);
    return H2_PAL_OK;
}
static int release_handler(void *user, const h2_pal_system_event_t *value) {
    (void)user; (void)value;
    const h2_pal_system_event_api_t *api = h2_jieli_wl82_platform_system_event_api();
    h2_pal_system_event_deinit(api);
    assert(atomic_load(&live) == 1); /* this operation still borrows the lock */
    return H2_PAL_OK;
}
static void *initialize(void *unused) {
    (void)unused;
    assert(h2_pal_system_event_init(h2_jieli_wl82_platform_system_event_api()) == H2_PAL_OK);
    return NULL;
}
static void *churn(void *unused) {
    (void)unused;
    const h2_pal_system_event_api_t *api = h2_jieli_wl82_platform_system_event_api();
    for (unsigned i = 0; i < 2000; ++i) {
        int rc;
        do {
            rc = h2_pal_system_event_init(api);
            if (rc == H2_PAL_ERR_BUSY) sched_yield();
        } while (rc == H2_PAL_ERR_BUSY);
        assert(rc == H2_PAL_OK);
        assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
        h2_pal_system_event_deinit(api);
    }
    return NULL;
}
static atomic_int dispatch_entered, dispatch_exit, unsubscribe_entered, unsubscribe_done;
static h2_pal_system_event_subscription_t *retired;
static _Thread_local int task_token;
const void *h2_jieli_sdk_task_current(void) { return &task_token; }
void h2_jieli_sdk_sleep_ms(uint32_t ms) {
    struct timespec duration = {0, (long)ms * 1000000L};
    nanosleep(&duration, NULL);
}
static int blocked_handler(void *user, const h2_pal_system_event_t *value) {
    (void)value;
    atomic_store(&dispatch_entered, 1);
    while (!atomic_load(&dispatch_exit)) sched_yield();
    assert(*(int *)user == 42);
    return H2_PAL_OK;
}
static void *post_blocked(void *unused) {
    (void)unused;
    assert(h2_pal_system_event_post(h2_jieli_wl82_platform_system_event_api(), &event, 0) == H2_PAL_OK);
    return NULL;
}
static void *retire(void *user) {
    atomic_store(&unsubscribe_entered, 1);
    h2_pal_system_event_unsubscribe(h2_jieli_wl82_platform_system_event_api(), retired);
    atomic_store(&unsubscribe_done, 1);
    free(user);
    return NULL;
}
static int self_remove(void *user, const h2_pal_system_event_t *value) {
    (void)user; (void)value;
    h2_pal_system_event_unsubscribe(h2_jieli_wl82_platform_system_event_api(), retired);
    atomic_fetch_add(&calls, 1);
    return H2_PAL_OK;
}
static void test_unsubscribe(int closing) {
    atomic_store(&dispatch_entered, 0);
    atomic_store(&dispatch_exit, 0);
    atomic_store(&unsubscribe_entered, 0);
    atomic_store(&unsubscribe_done, 0);
    const h2_pal_system_event_api_t *api = h2_jieli_wl82_platform_system_event_api();
    assert(h2_pal_system_event_init(api) == H2_PAL_OK);
    int *user = malloc(sizeof(*user)); *user = 42;
    assert(h2_pal_system_event_subscribe(api, event.type, blocked_handler, user, &retired) == H2_PAL_OK);
    pthread_t poster, remover;
    assert(pthread_create(&poster, NULL, post_blocked, NULL) == 0);
    while (!atomic_load(&dispatch_entered)) sched_yield();
    /* Closing must not let external retirement free a live callback context. */
    if (closing) h2_pal_system_event_deinit(api);
    assert(pthread_create(&remover, NULL, retire, user) == 0);
    while (!atomic_load(&unsubscribe_entered)) sched_yield();
    h2_jieli_sdk_sleep_ms(30);
    assert(!atomic_load(&unsubscribe_done));
    atomic_store(&dispatch_exit, 1);
    assert(pthread_join(poster, NULL) == 0);
    assert(pthread_join(remover, NULL) == 0);
    assert(atomic_load(&unsubscribe_done));
    if (closing) {
        assert(atomic_load(&live) == 0);
        assert(h2_pal_system_event_init(api) == H2_PAL_OK);
    }
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
    assert(h2_pal_system_event_subscribe(api, event.type, self_remove, NULL, &retired) == H2_PAL_OK);
    int before = atomic_load(&calls);
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
    assert(atomic_load(&calls) == before + 1);
    h2_pal_system_event_deinit(api);
    atomic_store(&calls, 0);
}
static int fanout_release_handler(void *user, const h2_pal_system_event_t *value) {
    ++*(unsigned *)user;
    return release_handler(NULL, value);
}
static int fanout_later_handler(void *user, const h2_pal_system_event_t *value) {
    (void)value;
    ++*(unsigned *)user;
    assert(atomic_load(&live) == 1);
    return H2_PAL_OK;
}
static void test_fanout_after_last_deinit(void) {
    const h2_pal_system_event_api_t *api = h2_jieli_wl82_platform_system_event_api();
    h2_pal_system_event_subscription_t *first, *later;
    unsigned first_calls = 0, later_calls = 0;
    assert(h2_pal_system_event_init(api) == H2_PAL_OK);
    assert(h2_pal_system_event_subscribe(api, event.type, fanout_release_handler,
                                       &first_calls, &first) == H2_PAL_OK);
    assert(h2_pal_system_event_subscribe(api, event.type, fanout_later_handler,
                                       &later_calls, &later) == H2_PAL_OK);
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
    assert(first_calls == 1);
    assert(later_calls == 1);
    assert(atomic_load(&live) == 0);
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_system_event_init(api) == H2_PAL_OK);
    h2_pal_system_event_deinit(api);
    assert(atomic_load(&live) == 0);
}
static void test_owners(void) {
    const h2_pal_system_event_api_t *api = h2_jieli_wl82_platform_system_event_api();
    h2_pal_system_event_subscription_t *sub;
    assert(h2_pal_system_event_init(api) == H2_PAL_OK);
    assert(h2_pal_system_event_subscribe(api, event.type, owner_handler, NULL, &sub) == H2_PAL_OK);
    assert(h2_pal_system_event_init(api) == H2_PAL_OK);
    h2_pal_system_event_deinit(api);
    assert(atomic_load(&live) == 1);
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
    assert(atomic_load(&calls) == 1);
    h2_pal_system_event_unsubscribe(api, sub);
    h2_pal_system_event_deinit(api);
    assert(atomic_load(&live) == 0);
    h2_pal_system_event_deinit(api);
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_ERR_INVALID_STATE);

    /* Public owner limit: overflow neither wraps nor acquires another owner. */
    for (unsigned i = 0; i < 16383; ++i)
        assert(h2_pal_system_event_init(api) == H2_PAL_OK);
    assert(h2_pal_system_event_init(api) == H2_PAL_ERR_FULL);
    for (unsigned i = 1; i < 16383; ++i) h2_pal_system_event_deinit(api);
    assert(atomic_load(&live) == 1);
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
    h2_pal_system_event_deinit(api);
    assert(atomic_load(&live) == 0);

    /* Last-owner release inside a callback defers destruction until post exits. */
    for (unsigned owners = 1; owners <= 2; ++owners) {
        for (unsigned i = 0; i < owners; ++i)
            assert(h2_pal_system_event_init(api) == H2_PAL_OK);
        assert(h2_pal_system_event_subscribe(api, event.type, release_handler, NULL, &sub) == H2_PAL_OK);
        assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
        assert(atomic_load(&live) == (int)(owners - 1));
        if (owners == 2) {
            h2_pal_system_event_unsubscribe(api, sub);
            h2_pal_system_event_deinit(api);
        }
        assert(atomic_load(&live) == 0);
    }

    atomic_store(&park_create, 1);
    pthread_t initializer;
    assert(pthread_create(&initializer, NULL, initialize, NULL) == 0);
    while (!atomic_load(&create_parked)) sched_yield();
    assert(h2_pal_system_event_init(api) == H2_PAL_ERR_BUSY);
    h2_pal_system_event_deinit(api); /* no successful owner exists yet */
    atomic_store(&resume_create, 1);
    assert(pthread_join(initializer, NULL) == 0);
    atomic_store(&park_create, 0);
    h2_pal_system_event_deinit(api);
    assert(atomic_load(&live) == 0);

    /* Race both additional owners and the zero-owner/INITIALIZING boundary. */
    for (unsigned anchored = 0; anchored <= 1; ++anchored) {
        if (anchored) {
            assert(h2_pal_system_event_init(api) == H2_PAL_OK);
            assert(h2_pal_system_event_subscribe(api, event.type, owner_handler, NULL, &sub) == H2_PAL_OK);
        }
        pthread_t workers[4];
        for (unsigned i = 0; i < 4; ++i)
            assert(pthread_create(&workers[i], NULL, churn, NULL) == 0);
        for (unsigned i = 0; i < 4; ++i) assert(pthread_join(workers[i], NULL) == 0);
        if (anchored) {
            assert(atomic_load(&calls) == 8001);
            h2_pal_system_event_unsubscribe(api, sub);
            h2_pal_system_event_deinit(api);
        }
        assert(atomic_load(&live) == 0);
    }
}
int main(void) {
    test_unsubscribe(0);
    test_unsubscribe(1);
    test_fanout_after_last_deinit();
    test_owners();
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
