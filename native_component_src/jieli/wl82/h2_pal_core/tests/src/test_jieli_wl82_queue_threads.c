#define _POSIX_C_SOURCE 200809L
#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

struct h2_jieli_sdk_mutex { pthread_mutex_t lock; };
struct h2_jieli_sdk_sem {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned count;
};
static atomic_int allocations, waiting, polling;

void *h2_jieli_sdk_malloc(size_t size) {
    void *p = calloc(1, size);
    if (p != NULL) atomic_fetch_add(&allocations, 1);
    return p;
}
void h2_jieli_sdk_free(void *p) {
    if (p != NULL) atomic_fetch_sub(&allocations, 1);
    free(p);
}
h2_jieli_sdk_mutex_t *h2_jieli_sdk_mutex_create(void) {
    h2_jieli_sdk_mutex_t *m = h2_jieli_sdk_malloc(sizeof(*m));
    assert(m != NULL && pthread_mutex_init(&m->lock, NULL) == 0);
    return m;
}
void h2_jieli_sdk_mutex_destroy(h2_jieli_sdk_mutex_t *m) {
    assert(pthread_mutex_destroy(&m->lock) == 0);
    h2_jieli_sdk_free(m);
}
int h2_jieli_sdk_mutex_lock(h2_jieli_sdk_mutex_t *m, uint32_t timeout) {
    (void)timeout;
    return pthread_mutex_lock(&m->lock) == 0 ? 0 : -1;
}
int h2_jieli_sdk_mutex_unlock(h2_jieli_sdk_mutex_t *m) {
    return pthread_mutex_unlock(&m->lock) == 0 ? 0 : -1;
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
    atomic_fetch_add(&waiting, 1);
    int rc = 0;
    while (s->count == 0u && rc == 0) {
        rc = timeout == 0u ? ETIMEDOUT :
            pthread_cond_timedwait(&s->changed, &s->lock, &deadline);
    }
    atomic_fetch_sub(&waiting, 1);
    if (s->count != 0u) { --s->count; rc = 0; }
    assert(pthread_mutex_unlock(&s->lock) == 0);
    assert(rc == 0 || rc == ETIMEDOUT);
    return rc == 0 ? 0 : 1;
}
typedef struct worker { h2_pal_queue_t *queue; int send, result; } worker_t;
static void *run_worker(void *arg) {
    worker_t *w = arg;
    int value = 91;
    const h2_pal_queue_api_t *api = h2_jieli_wl82_platform_queue_api();
    w->result = w->send ? h2_pal_queue_send(api, w->queue, &value, 10000u)
                       : h2_pal_queue_recv(api, w->queue, &value, 10000u);
    return NULL;
}
static void *poll_sender(void *arg) {
    worker_t *w = arg;
    int value = 91;
    const h2_pal_queue_api_t *api = h2_jieli_wl82_platform_queue_api();
    for (unsigned i = 0; i < 10000000u; ++i) {
        w->result = h2_pal_queue_send(api, w->queue, &value, 0u);
        if (w->result == H2_PAL_ERR_CLOSED) return NULL;
        assert(w->result == H2_PAL_ERR_FULL);
        atomic_fetch_add(&polling, 1);
    }
    assert(!"polling sender did not observe close");
    return NULL;
}
static void await_waiters(int count) {
    for (unsigned i = 0; i < 5000u; ++i) {
        if (atomic_load(&waiting) == count) return;
        const struct timespec delay = {0, 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    assert(!"queue waiters did not reach barrier");
}
int main(void) {
    const h2_pal_queue_api_t *api = h2_jieli_wl82_platform_queue_api();
    const h2_pal_queue_config_t config = {.item_size = sizeof(int), .item_count = 1u};
    for (unsigned iteration = 0; iteration < 50u; ++iteration) {
        for (int send = 0; send <= 1; ++send) {
            h2_pal_queue_t *queue = NULL;
            assert(h2_pal_queue_create(api, &config, &queue) == H2_PAL_OK);
            int value = 7;
            if (send) assert(h2_pal_queue_send(api, queue, &value, 0u) == H2_PAL_OK);
            worker_t workers[3];
            pthread_t threads[3];
            for (int i = 0; i < 3; ++i) {
                workers[i] = (worker_t){queue, send, -1};
                assert(pthread_create(&threads[i], NULL, run_worker, &workers[i]) == 0);
            }
            await_waiters(3);
            worker_t poll = {queue, 1, -1};
            pthread_t poll_thread;
            if (send) {
                atomic_store(&polling, 0);
                assert(pthread_create(&poll_thread, NULL, poll_sender, &poll) == 0);
                while (atomic_load(&polling) < 100) {
                    const struct timespec delay = {0, 1000000L};
                    (void)nanosleep(&delay, NULL);
                }
            }
            for (int i = 0; i < 20; ++i) assert(h2_pal_queue_close(api, queue) == H2_PAL_OK);
            if (send) {
                assert(pthread_join(poll_thread, NULL) == 0);
                assert(poll.result == H2_PAL_ERR_CLOSED);
            }
            for (int i = 0; i < 3; ++i) {
                assert(pthread_join(threads[i], NULL) == 0);
                assert(workers[i].result == H2_PAL_ERR_CLOSED);
            }
            assert(h2_pal_queue_send(api, queue, &value, 0u) == H2_PAL_ERR_CLOSED);
            if (send) {
                assert(h2_pal_queue_recv(api, queue, &value, 0u) == H2_PAL_OK);
                assert(value == 7);
            }
            assert(h2_pal_queue_recv(api, queue, &value, 0u) == H2_PAL_ERR_CLOSED);
            h2_pal_queue_destroy(api, queue);
            assert(atomic_load(&allocations) == 0);
        }
    }
    return 0;
}
