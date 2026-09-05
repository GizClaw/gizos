#define _POSIX_C_SOURCE 200809L
#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

struct h2_jieli_sdk_mutex { pthread_mutex_t native; };
struct h2_jieli_sdk_sem {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned count;
};
static atomic_int allocations, entered, parked, hold_notified;

void *h2_jieli_sdk_malloc(size_t size) {
    void *p = calloc(1, size);
    if (p != NULL) atomic_fetch_add(&allocations, 1);
    return p;
}
void h2_jieli_sdk_free(void *p) {
    if (p != NULL) atomic_fetch_sub(&allocations, 1);
    free(p);
}
void h2_jieli_sdk_sleep_ms(uint32_t ms) {
    struct timespec delay = {ms / 1000u, (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&delay, &delay) != 0) assert(errno == EINTR);
}
const void *h2_jieli_sdk_task_current(void) {
    static _Thread_local int identity;
    return &identity;
}
h2_jieli_sdk_mutex_t *h2_jieli_sdk_mutex_create(void) {
    h2_jieli_sdk_mutex_t *m = h2_jieli_sdk_malloc(sizeof(*m));
    assert(m != NULL && pthread_mutex_init(&m->native, NULL) == 0);
    return m;
}
void h2_jieli_sdk_mutex_destroy(h2_jieli_sdk_mutex_t *m) {
    assert(pthread_mutex_destroy(&m->native) == 0);
    h2_jieli_sdk_free(m);
}
int h2_jieli_sdk_mutex_lock(h2_jieli_sdk_mutex_t *m, uint32_t timeout) {
    int rc = timeout == 0u ? pthread_mutex_trylock(&m->native)
                           : pthread_mutex_lock(&m->native);
    return rc == 0 ? 0 : (rc == EBUSY ? 1 : -1);
}
int h2_jieli_sdk_mutex_unlock(h2_jieli_sdk_mutex_t *m) {
    return pthread_mutex_unlock(&m->native) == 0 ? 0 : -1;
}
h2_jieli_sdk_sem_t *h2_jieli_sdk_sem_create(uint32_t count) {
    h2_jieli_sdk_sem_t *s = h2_jieli_sdk_malloc(sizeof(*s));
    assert(s != NULL);
    assert(pthread_mutex_init(&s->lock, NULL) == 0);
    assert(pthread_cond_init(&s->changed, NULL) == 0);
    s->count = count;
    return s;
}
void h2_jieli_sdk_sem_destroy(h2_jieli_sdk_sem_t *s) {
    assert(pthread_mutex_destroy(&s->lock) == 0);
    assert(pthread_cond_destroy(&s->changed) == 0);
    h2_jieli_sdk_free(s);
}
int h2_jieli_sdk_sem_give(h2_jieli_sdk_sem_t *s) {
    assert(pthread_mutex_lock(&s->lock) == 0);
    ++s->count;
    assert(pthread_cond_signal(&s->changed) == 0);
    assert(pthread_mutex_unlock(&s->lock) == 0);
    return 0;
}
int h2_jieli_sdk_sem_take(h2_jieli_sdk_sem_t *s, uint32_t timeout) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += timeout / 1000u;
    deadline.tv_nsec += (long)(timeout % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    assert(pthread_mutex_lock(&s->lock) == 0);
    atomic_fetch_add(&entered, 1);
    int rc = 0;
    while (s->count == 0u && rc == 0) {
        rc = timeout == 0u ? ETIMEDOUT :
            pthread_cond_timedwait(&s->changed, &s->lock, &deadline);
    }
    if (s->count != 0u) { --s->count; rc = 0; }
    assert(pthread_mutex_unlock(&s->lock) == 0);
    assert(rc == 0 || rc == ETIMEDOUT);
    if (rc == 0) {
        atomic_fetch_add(&parked, 1);
        /* Hold a notified waiter before PAL unlink, allowing a late waiter
         * and destroy to race the precise notification-retirement interval. */
        while (atomic_load(&hold_notified)) h2_jieli_sdk_sleep_ms(1u);
    }
    return rc == 0 ? 0 : 1;
}

typedef struct worker { h2_pal_cond_t *cond; uint32_t timeout; int result; } worker_t;
static void *wait_thread(void *arg) {
    worker_t *w = arg;
    const h2_pal_sync_api_t *api = h2_jieli_wl82_platform_sync_api();
    h2_pal_mutex_t *mutex = NULL;
    const h2_pal_mutex_config_t config = {.name = "worker"};
    assert(h2_pal_mutex_create(api, &config, &mutex) == H2_PAL_OK);
    assert(h2_pal_mutex_lock(api, mutex) == H2_PAL_OK);
    w->result = h2_pal_cond_wait(api, w->cond, mutex, w->timeout);
    assert(h2_pal_mutex_unlock(api, mutex) == H2_PAL_OK);
    assert(h2_pal_mutex_destroy(api, mutex) == H2_PAL_OK);
    return NULL;
}
static void await_count(atomic_int *counter, int value) {
    for (unsigned i = 0; i < 5000u; ++i) {
        if (atomic_load(counter) == value) return;
        h2_jieli_sdk_sleep_ms(1u);
    }
    assert(!"worker did not reach barrier");
}
int main(void) {
    const h2_pal_sync_api_t *api = h2_jieli_wl82_platform_sync_api();
    const h2_pal_cond_config_t config = {.name = "threaded-cond"};
    for (unsigned iteration = 0; iteration < 20u; ++iteration) {
        h2_pal_cond_t *cond = NULL;
        assert(h2_pal_cond_create(api, &config, &cond) == H2_PAL_OK);
        atomic_store(&entered, 0);
        atomic_store(&parked, 0);
        atomic_store(&hold_notified, 1);
        worker_t a = {cond, 10000u, -1}, b = {cond, 10000u, -1};
        pthread_t first, second;
        assert(pthread_create(&first, NULL, wait_thread, &a) == 0);
        assert(pthread_create(&second, NULL, wait_thread, &b) == 0);
        await_count(&entered, 2);
        assert(h2_pal_cond_signal(api, cond) == H2_PAL_OK);
        await_count(&parked, 1);
        assert(h2_pal_cond_destroy(api, cond) == H2_PAL_ERR_INVALID_STATE);
        worker_t late = {cond, 5u, -1};
        wait_thread(&late);
        assert(late.result == H2_PAL_ERR_TIMEOUT);
        assert(h2_pal_cond_broadcast(api, cond) == H2_PAL_OK);
        await_count(&parked, 2);
        assert(h2_pal_cond_destroy(api, cond) == H2_PAL_ERR_INVALID_STATE);
        atomic_store(&hold_notified, 0);
        assert(pthread_join(first, NULL) == 0);
        assert(pthread_join(second, NULL) == 0);
        assert(a.result == H2_PAL_OK && b.result == H2_PAL_OK);
        assert(h2_pal_cond_destroy(api, cond) == H2_PAL_OK);
        assert(atomic_load(&allocations) == 0);
    }
    return 0;
}
