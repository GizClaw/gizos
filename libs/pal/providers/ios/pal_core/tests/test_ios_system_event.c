#include "h2_ios_platform.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <time.h>

enum { kSubscriptionCapacity = 48 };

typedef struct event_counter {
    size_t calls;
    int result;
} event_counter_t;

typedef struct blocking_handler {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int entered;
    int release;
} blocking_handler_t;

typedef struct post_args {
    const h2_pal_system_event_api_t *events;
    int result;
} post_args_t;

typedef struct unsubscribe_args {
    const h2_pal_system_event_api_t *events;
    h2_pal_system_event_subscription_t *subscription;
    atomic_int started;
    atomic_int done;
} unsubscribe_args_t;

typedef struct self_unsubscribe_handler {
    const h2_pal_system_event_api_t *events;
    h2_pal_system_event_subscription_t *subscription;
    size_t calls;
} self_unsubscribe_handler_t;

static int count_event(void *user, const h2_pal_system_event_t *event) {
    event_counter_t *counter = user;
    assert(event != NULL);
    ++counter->calls;
    return counter->result;
}

static int block_event(void *user, const h2_pal_system_event_t *event) {
    blocking_handler_t *blocking = user;
    assert(event != NULL);
    pthread_mutex_lock(&blocking->mutex);
    blocking->entered = 1;
    pthread_cond_broadcast(&blocking->condition);
    while (blocking->release == 0) {
        pthread_cond_wait(&blocking->condition, &blocking->mutex);
    }
    pthread_mutex_unlock(&blocking->mutex);
    return H2_PAL_OK;
}

static int unsubscribe_self(void *user, const h2_pal_system_event_t *event) {
    self_unsubscribe_handler_t *handler = user;
    assert(event != NULL);
    ++handler->calls;
    h2_pal_system_event_unsubscribe(handler->events, handler->subscription);
    return H2_PAL_OK;
}

static h2_pal_system_event_t test_event(void) {
    return (h2_pal_system_event_t){
        .type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
    };
}

static void *post_event(void *user) {
    post_args_t *args = user;
    const h2_pal_system_event_t event = test_event();
    args->result = h2_pal_system_event_post(args->events, &event, 0u);
    return NULL;
}

static void *unsubscribe_event(void *user) {
    unsubscribe_args_t *args = user;
    atomic_store_explicit(&args->started, 1, memory_order_release);
    h2_pal_system_event_unsubscribe(args->events, args->subscription);
    atomic_store_explicit(&args->done, 1, memory_order_release);
    return NULL;
}

static void test_subscription_capacity(
    const h2_pal_system_event_api_t *events) {
    h2_pal_system_event_subscription_t
        *subscriptions[kSubscriptionCapacity] = {0};
    event_counter_t counters[kSubscriptionCapacity] = {0};
    for (size_t i = 0u; i < kSubscriptionCapacity; ++i) {
        assert(h2_pal_system_event_subscribe(
                   events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
                   count_event, &counters[i], &subscriptions[i]) == H2_PAL_OK);
        assert(subscriptions[i] != NULL);
    }
    h2_pal_system_event_subscription_t *overflow = NULL;
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
               count_event, &counters[0], &overflow) == H2_PAL_ERR_FULL);
    assert(overflow == NULL);

    counters[7].result = H2_PAL_ERR_IO;
    const h2_pal_system_event_t event = test_event();
    assert(h2_pal_system_event_post(events, &event, 0u) == H2_PAL_ERR_IO);
    for (size_t i = 0u; i < kSubscriptionCapacity; ++i) {
        assert(counters[i].calls == 1u);
        h2_pal_system_event_unsubscribe(events, subscriptions[i]);
    }
}

static void test_unsubscribe_drains_in_flight(
    const h2_pal_system_event_api_t *events) {
    blocking_handler_t blocking = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };
    h2_pal_system_event_subscription_t *subscription = NULL;
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
               block_event, &blocking, &subscription) == H2_PAL_OK);

    post_args_t post = {.events = events, .result = H2_PAL_ERR_INVALID_STATE};
    pthread_t posting_thread;
    assert(pthread_create(&posting_thread, NULL, post_event, &post) == 0);
    pthread_mutex_lock(&blocking.mutex);
    while (blocking.entered == 0) {
        pthread_cond_wait(&blocking.condition, &blocking.mutex);
    }
    pthread_mutex_unlock(&blocking.mutex);

    unsubscribe_args_t unsubscribe = {
        .events = events,
        .subscription = subscription,
    };
    atomic_init(&unsubscribe.started, 0);
    atomic_init(&unsubscribe.done, 0);
    pthread_t unsubscribe_thread;
    assert(pthread_create(&unsubscribe_thread, NULL, unsubscribe_event,
                          &unsubscribe) == 0);
    while (atomic_load_explicit(&unsubscribe.started,
                                memory_order_acquire) == 0) {
    }
    const struct timespec drain_check = {.tv_nsec = 10000000L};
    assert(nanosleep(&drain_check, NULL) == 0);
    assert(atomic_load_explicit(&unsubscribe.done, memory_order_acquire) == 0);

    pthread_mutex_lock(&blocking.mutex);
    blocking.release = 1;
    pthread_cond_broadcast(&blocking.condition);
    pthread_mutex_unlock(&blocking.mutex);
    assert(pthread_join(posting_thread, NULL) == 0);
    assert(pthread_join(unsubscribe_thread, NULL) == 0);
    assert(post.result == H2_PAL_OK);
    assert(atomic_load_explicit(&unsubscribe.done, memory_order_acquire) == 1);
    assert(pthread_cond_destroy(&blocking.condition) == 0);
    assert(pthread_mutex_destroy(&blocking.mutex) == 0);
}

static void test_handler_can_unsubscribe_itself(
    const h2_pal_system_event_api_t *events) {
    self_unsubscribe_handler_t handler = {.events = events};
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
               unsubscribe_self, &handler, &handler.subscription) == H2_PAL_OK);
    const h2_pal_system_event_t event = test_event();
    assert(h2_pal_system_event_post(events, &event, 0u) == H2_PAL_OK);
    assert(handler.calls == 1u);
    assert(h2_pal_system_event_post(events, &event, 0u) == H2_PAL_OK);
    assert(handler.calls == 1u);

    h2_pal_system_event_subscription_t *replacement = NULL;
    event_counter_t counter = {0};
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
               count_event, &counter, &replacement) == H2_PAL_OK);
    h2_pal_system_event_unsubscribe(events, replacement);
}

void h2_ios_test_system_event(void) {
    const h2_pal_system_event_api_t *events = h2_ios_system_event_api();
    assert(events != NULL);
    const h2_pal_system_event_t event = test_event();
    assert(h2_pal_system_event_post(events, &event, 0u) ==
           H2_PAL_ERR_INVALID_STATE);
    for (size_t iteration = 0u; iteration < 2u; ++iteration) {
        assert(h2_pal_system_event_init(events) == H2_PAL_OK);
        assert(h2_pal_system_event_init(events) == H2_PAL_OK);
        test_handler_can_unsubscribe_itself(events);
        test_subscription_capacity(events);
        test_unsubscribe_drains_in_flight(events);
        h2_pal_system_event_deinit(events);
        assert(h2_pal_system_event_post(events, &event, 0u) ==
               H2_PAL_ERR_INVALID_STATE);
    }
}
