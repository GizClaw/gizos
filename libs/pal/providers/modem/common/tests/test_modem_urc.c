#define _POSIX_C_SOURCE 200809L
#include "h2_modem_urc.h"
#include "h2_quectel_modem.h"
#include "h2_simcom_modem.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct h2_pal_task { pthread_t thread; h2_pal_task_entry_t entry; void *ctx; };
struct h2_pal_mutex { pthread_mutex_t mutex; };
struct h2_pal_queue {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    size_t size, capacity, head, count;
    int closed;
    unsigned char *items;
};
static unsigned live_tasks, live_queues, live_mutexes;
static int fail_start, fail_join, fail_queue;

static void *task_entry(void *arg) {
    h2_pal_task_t *task = arg;
    task->entry(task->ctx);
    return NULL;
}
static int task_start(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *ctx, h2_pal_task_t **out) {
    (void)user;
    assert(strcmp(options->name, "$modem/urc") == 0);
    assert(options->min_stack_size >= 4096u);
    if (fail_start) { return H2_PAL_ERR_TASK; }
    *out = calloc(1, sizeof(**out));
    assert(*out != NULL);
    (*out)->entry = entry;
    (*out)->ctx = ctx;
    assert(pthread_create(&(*out)->thread, NULL, task_entry, *out) == 0);
    live_tasks++;
    return H2_PAL_OK;
}
static int task_join(void *user, h2_pal_task_t *task) {
    (void)user;
    if (fail_join) { return H2_PAL_ERR_TASK; }
    assert(pthread_join(task->thread, NULL) == 0);
    free(task);
    live_tasks--;
    return H2_PAL_OK;
}
static const h2_pal_task_vtable_t task_vtable = {.start = task_start, .join = task_join};
static const h2_pal_task_api_t tasks = {.vtable = &task_vtable};

static int queue_create(void *user, const h2_pal_queue_config_t *config, h2_pal_queue_t **out) {
    (void)user;
    if (fail_queue) { return H2_PAL_ERR_NO_MEMORY; }
    *out = calloc(1, sizeof(**out));
    assert(*out != NULL);
    h2_pal_queue_t *q = *out;
    q->size = config->item_size;
    q->capacity = config->item_count;
    q->items = calloc(q->capacity, q->size);
    assert(q->items != NULL);
    assert(pthread_mutex_init(&q->mutex, NULL) == 0);
    assert(pthread_cond_init(&q->changed, NULL) == 0);
    live_queues++;
    return H2_PAL_OK;
}
static void queue_destroy(void *user, h2_pal_queue_t *q) {
    (void)user;
    assert(pthread_cond_destroy(&q->changed) == 0);
    assert(pthread_mutex_destroy(&q->mutex) == 0);
    free(q->items);
    free(q);
    live_queues--;
}
static int queue_send(void *user, h2_pal_queue_t *q, const void *item, uint32_t timeout_ms) {
    (void)user;
    assert(timeout_ms == H2_PAL_QUEUE_NO_WAIT);
    assert(pthread_mutex_lock(&q->mutex) == 0);
    int rc = H2_PAL_OK;
    if (q->closed) {
        rc = H2_PAL_ERR_CLOSED;
    } else if (q->count == q->capacity) {
        rc = H2_PAL_ERR_TIMEOUT;
    } else {
        size_t tail = (q->head + q->count) % q->capacity;
        memcpy(q->items + tail * q->size, item, q->size);
        q->count++;
        assert(pthread_cond_signal(&q->changed) == 0);
    }
    assert(pthread_mutex_unlock(&q->mutex) == 0);
    return rc;
}
static int queue_recv(void *user, h2_pal_queue_t *q, void *item, uint32_t timeout_ms) {
    (void)user;
    assert(timeout_ms == H2_PAL_QUEUE_WAIT_FOREVER);
    assert(pthread_mutex_lock(&q->mutex) == 0);
    while (!q->closed && q->count == 0u) {
        assert(pthread_cond_wait(&q->changed, &q->mutex) == 0);
    }
    int rc = H2_PAL_ERR_CLOSED;
    if (!q->closed) {
        memcpy(item, q->items + q->head * q->size, q->size);
        q->head = (q->head + 1u) % q->capacity;
        q->count--;
        rc = H2_PAL_OK;
    }
    assert(pthread_mutex_unlock(&q->mutex) == 0);
    return rc;
}
static int queue_close(void *user, h2_pal_queue_t *q) {
    (void)user;
    assert(pthread_mutex_lock(&q->mutex) == 0);
    q->closed = 1;
    assert(pthread_cond_broadcast(&q->changed) == 0);
    assert(pthread_mutex_unlock(&q->mutex) == 0);
    return H2_PAL_OK;
}
static const h2_pal_queue_vtable_t queue_vtable = {
    .create = queue_create, .destroy = queue_destroy, .send = queue_send,
    .recv = queue_recv, .close = queue_close,
};
static const h2_pal_queue_api_t queues = {.vtable = &queue_vtable};

static h2_pal_result_t mutex_create(void *user, const h2_pal_mutex_config_t *config, h2_pal_mutex_t **out) {
    (void)user;
    *out = calloc(1, sizeof(**out));
    assert(*out != NULL);
    pthread_mutexattr_t attr;
    assert(pthread_mutexattr_init(&attr) == 0);
    if ((config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u) {
        assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
    }
    assert(pthread_mutex_init(&(*out)->mutex, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);
    live_mutexes++;
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_destroy(&mutex->mutex) == 0);
    free(mutex);
    live_mutexes--;
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_lock(&mutex->mutex) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_unlock(&mutex->mutex) == 0);
    return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = mutex_create, .destroy_mutex = mutex_destroy,
    .lock_mutex = mutex_lock, .unlock_mutex = mutex_unlock,
};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};

typedef struct fixture {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int entered, released, received, rx_returned;
    int vendor, timeout;
    h2_quectel_modem_t quectel;
    h2_simcom_modem_t simcom;
    pthread_t caller;
    char lines[H2_MODEM_URC_QUEUE_SIZE + 1u][H2_MODEM_URC_LINE_MAX];
} fixture_t;

static void init_fixture(fixture_t *f) {
    memset(f, 0, sizeof(*f));
    f->caller = pthread_self();
    assert(pthread_mutex_init(&f->mutex, NULL) == 0);
    assert(pthread_cond_init(&f->changed, NULL) == 0);
}
static void finish_fixture(fixture_t *f) {
    assert(pthread_cond_destroy(&f->changed) == 0);
    assert(pthread_mutex_destroy(&f->mutex) == 0);
    assert(live_tasks == 0u && live_queues == 0u && live_mutexes == 0u);
}
static void wait_value(fixture_t *f, int *value, int target) {
    struct timespec until;
    assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec += 2;
    assert(pthread_mutex_lock(&f->mutex) == 0);
    while (*value < target) {
        assert(pthread_cond_timedwait(&f->changed, &f->mutex, &until) == 0);
    }
    assert(pthread_mutex_unlock(&f->mutex) == 0);
}
static void collect(void *user, const char *line) {
    fixture_t *f = user;
    assert(!pthread_equal(f->caller, pthread_self()));
    assert(pthread_mutex_lock(&f->mutex) == 0);
    f->entered = 1;
    assert(pthread_cond_broadcast(&f->changed) == 0);
    while (!f->released) { assert(pthread_cond_wait(&f->changed, &f->mutex) == 0); }
    assert(f->received < (int)(H2_MODEM_URC_QUEUE_SIZE + 1u));
    strcpy(f->lines[f->received++], line);
    assert(pthread_cond_broadcast(&f->changed) == 0);
    assert(pthread_mutex_unlock(&f->mutex) == 0);
}
static void test_queue_and_lifecycle(void) {
    fixture_t f;
    init_fixture(&f);
    h2_modem_urc_worker_t worker = {0};
    assert(h2_modem_urc_start(&worker, &tasks, &queues, NULL, collect, &f) == H2_PAL_OK);
    assert(h2_modem_urc_post(&worker, "first") == H2_PAL_OK);
    wait_value(&f, &f.entered, 1);
    for (unsigned i = 0u; i < H2_MODEM_URC_QUEUE_SIZE; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "line-%u", i);
        assert(h2_modem_urc_post(&worker, line) == H2_PAL_OK);
        memset(line, 'x', sizeof(line)); /* The worker owns a copy. */
    }
    assert(h2_modem_urc_post(&worker, "overflow") == H2_PAL_ERR_FULL);
    char too_long[H2_MODEM_URC_LINE_MAX + 1u];
    memset(too_long, 'x', sizeof(too_long));
    too_long[sizeof(too_long) - 1u] = '\0';
    assert(h2_modem_urc_post(&worker, too_long) == H2_PAL_ERR_TRUNCATED);
    assert(pthread_mutex_lock(&f.mutex) == 0);
    f.released = 1;
    assert(pthread_cond_broadcast(&f.changed) == 0);
    assert(pthread_mutex_unlock(&f.mutex) == 0);
    wait_value(&f, &f.received, H2_MODEM_URC_QUEUE_SIZE + 1u);
    fail_join = 1;
    assert(h2_modem_urc_stop(&worker) == H2_PAL_ERR_TASK);
    assert(worker.task != NULL && worker.queue != NULL);
    assert(h2_modem_urc_post(&worker, "closed") == H2_PAL_ERR_CLOSED);
    fail_join = 0;
    assert(h2_modem_urc_stop(&worker) == H2_PAL_OK);
    assert(strcmp(f.lines[0], "first") == 0);
    for (unsigned i = 0u; i < H2_MODEM_URC_QUEUE_SIZE; ++i) {
        char expected[32];
        snprintf(expected, sizeof(expected), "line-%u", i);
        assert(strcmp(f.lines[i + 1u], expected) == 0);
    }
    assert(h2_modem_urc_post(&worker, "stopped") == H2_PAL_ERR_INVALID_STATE);
    assert(h2_modem_urc_stop(&worker) == H2_PAL_OK);
    fail_start = 1;
    assert(h2_modem_urc_start(&worker, &tasks, &queues, NULL, collect, &f) == H2_PAL_ERR_TASK);
    assert(worker.queue == NULL && worker.task == NULL);
    fail_start = 0;
    fail_queue = 1;
    assert(h2_modem_urc_start(&worker, &tasks, &queues, NULL, collect, &f) == H2_PAL_ERR_NO_MEMORY);
    fail_queue = 0;
    assert(h2_modem_urc_start(&worker, &tasks, &queues, NULL, collect, &f) == H2_PAL_OK);
    assert(h2_modem_urc_stop(&worker) == H2_PAL_OK); /* Idle receive wakes on close. */
    finish_fixture(&f);
}

static void *receive(void *user) {
    fixture_t *f = user;
    h2_pal_result_t rc = f->vendor == 0
        ? h2_quectel_post_urc_line(&f->quectel, "RING")
        : h2_simcom_post_urc_line(&f->simcom, "RING");
    assert(rc == H2_PAL_OK);
    assert(pthread_mutex_lock(&f->mutex) == 0);
    f->rx_returned = 1;
    assert(pthread_cond_broadcast(&f->changed) == 0);
    assert(pthread_mutex_unlock(&f->mutex) == 0);
    return NULL;
}
static h2_pal_result_t command(void *user, const char *cmd, char *response, size_t size, uint32_t timeout) {
    fixture_t *f = user;
    (void)timeout;
    if (strcmp(cmd, "AT+CSQ") == 0) {
        pthread_t rx;
        assert(pthread_create(&rx, NULL, receive, f) == 0);
        /* AT is holding the provider lock. RX must return before this command
         * can finish, regardless of whether the URC worker got scheduled. */
        wait_value(f, &f->rx_returned, 1);
        assert(pthread_join(rx, NULL) == 0);
        if (f->timeout) { return H2_PAL_ERR_TIMEOUT; }
        snprintf(response, size, "+CSQ: 20,0\r\nOK\r\n");
    } else {
        const char *text = "OK\r\n";
        if (strcmp(cmd, "AT+CGMI") == 0) { text = "SIMCOM\r\nOK\r\n"; }
        if (strcmp(cmd, "AT+CGMM") == 0) { text = "A7670E\r\nOK\r\n"; }
        if (strcmp(cmd, "AT+CGMR") == 0) { text = "TEST-REVISION\r\nOK\r\n"; }
        if (strcmp(cmd, "AT+CGSN") == 0) { text = "123456789012345\r\nOK\r\n"; }
        snprintf(response, size, "%s", text);
    }
    return H2_PAL_OK;
}
static int post_event(void *user, const h2_pal_system_event_t *event, uint32_t timeout) {
    fixture_t *f = user;
    (void)timeout;
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING) {
        assert(!pthread_equal(f->caller, pthread_self()));
        assert(pthread_mutex_lock(&f->mutex) == 0);
        f->received++;
        assert(pthread_cond_broadcast(&f->changed) == 0);
        assert(pthread_mutex_unlock(&f->mutex) == 0);
    }
    return H2_PAL_OK;
}
static const h2_pal_system_event_vtable_t event_vtable = {.post = post_event};

static void test_provider_receive(void) {
    for (int vendor = 0; vendor < 2; ++vendor) {
        for (int timeout = 0; timeout < 2; ++timeout) {
            fixture_t f;
            init_fixture(&f);
            f.vendor = vendor;
            f.timeout = timeout;
            h2_pal_system_event_api_t events = {.user = &f, .vtable = &event_vtable};
            h2_pal_modem_t *modem;
            if (vendor == 0) {
                h2_quectel_modem_config_t config = {
                    .transport_user = &f, .command = command, .sync_api = &sync_api,
                    .urc_task_api = &tasks, .urc_queue_api = &queues, .system_events = &events,
                };
                assert(h2_quectel_modem_init(&f.quectel, &config) == H2_PAL_OK);
                modem = &f.quectel.platform;
            } else {
                h2_simcom_modem_config_t config = {
                    .transport_user = &f, .command = command, .sync_api = &sync_api,
                    .urc_task_api = &tasks, .urc_queue_api = &queues, .system_events = &events,
                };
                assert(h2_simcom_modem_init(&f.simcom, &config) == H2_PAL_OK);
                modem = &f.simcom.platform;
            }
            assert(h2_pal_modem_open(modem, 0u) == H2_PAL_OK);
            h2_pal_modem_signal_t signal;
            assert(h2_pal_modem_get_signal(modem, &signal) == (timeout ? H2_PAL_ERR_TIMEOUT : H2_PAL_OK));
            wait_value(&f, &f.received, 1);
            fail_join = 1;
            assert((vendor == 0 ? h2_quectel_modem_deinit(&f.quectel)
                                : h2_simcom_modem_deinit(&f.simcom)) == H2_PAL_ERR_TASK);
            assert(live_mutexes == 1u && live_tasks == 1u && live_queues == 1u);
            fail_join = 0;
            assert((vendor == 0 ? h2_quectel_modem_deinit(&f.quectel)
                                : h2_simcom_modem_deinit(&f.simcom)) == H2_PAL_OK);
            finish_fixture(&f);
        }
    }
}

int main(void) {
    test_queue_and_lifecycle();
    test_provider_receive();
    return 0;
}
