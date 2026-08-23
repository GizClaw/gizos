#include "h2_jieli_br23_platform_core.h"
#include "h2_jieli_br23_sdk_port.h"
#include "h2_jieli_br23_atomic.h"

#include <string.h>

struct h2_pal_timer {
    h2_pal_timer_config_t config;
    uint16_t id;
    uint8_t running;
    /* Lifecycle flags shared between the owner task and the SDK dispatch.
     * `destroyed` is raised by destroy(); the storage is released only by
     * timer_reclaim(), a one-shot SDK timeout queued after the flag, so any
     * callback the SDK already dispatched before destroy() ran (or the one
     * destroy() is running inside of) finishes on valid memory. */
    volatile uint32_t destroyed;
    /* Task that registered the SDK timer, i.e. the task its callbacks are
     * dispatched to; destroy() must run there so the reclaim timeout lands
     * behind any fire already queued to it. */
    const void *owner_task;
    /* Set once the timer has been registered with the SDK, i.e. once a fire
     * can be queued to `owner_task` (the handle itself may be NULL before the
     * scheduler runs, so it cannot double as this flag). */
    uint8_t owned;
};

/* SDK timer callbacks run on the task that registered the timer, one after
 * another; the reclaim timeout is registered from destroy() on that same
 * task, so it is dispatched strictly after every previously queued fire.
 * Timers are therefore owned by one task: create/start records it and
 * destroy() from another task is rejected instead of freeing storage a
 * queued callback still reads. */
#define TIMER_RECLAIM_DELAY_MS 1u

static void timer_release(h2_pal_timer_t *timer)
{
    h2_jieli_sdk_free(timer);
}

static void timer_reclaim(void *ctx)
{
    timer_release((h2_pal_timer_t *)ctx);
}

static void timer_fire(void *ctx)
{
    h2_pal_timer_t *timer = (h2_pal_timer_t *)ctx;
    if (h2_jieli_atomic_load_u32(&timer->destroyed)) {
        /* Dispatched before destroy() ran; the reclaim timeout frees us. */
        return;
    }
    if ((timer->config.flags & H2_PAL_TIMER_FLAG_REPEAT) == 0u) {
        /* One-shot SDK timeouts release themselves after firing. */
        timer->running = 0u;
        timer->id = 0u;
    }
    /* The callback may destroy the timer; after this the storage belongs to
     * the reclaim timeout, so nothing here may touch `timer` again. */
    timer->config.cb(timer->config.cb_user, timer);
}

static h2_pal_result_t timer_start(void *user, h2_pal_timer_t *timer)
{
    (void)user;
    if (timer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (timer->owned && timer->owner_task != h2_jieli_sdk_task_current()) {
        /* A fire queued to the previous owner is not recalled by stop(), so
         * ownership can never move: re-arming from another task would let
         * that task order the reclaim behind its own callbacks only. The
         * check precedes the already-running fast path so a foreign task
         * never gets an OK that suggests it may drive this timer. */
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (timer->running) {
        return H2_PAL_OK;
    }
    timer->id = h2_jieli_sdk_timer_add(
        timer,
        timer_fire,
        timer->config.period_ms,
        (timer->config.flags & H2_PAL_TIMER_FLAG_REPEAT) != 0u);
    if (timer->id == 0u) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    timer->owner_task = h2_jieli_sdk_task_current();
    timer->owned = 1u;
    timer->running = 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t timer_stop(void *user, h2_pal_timer_t *timer)
{
    (void)user;
    if (timer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (timer->running) {
        h2_jieli_sdk_timer_del(timer->id, (timer->config.flags & H2_PAL_TIMER_FLAG_REPEAT) != 0u);
        timer->running = 0u;
        timer->id = 0u;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t timer_create(
    void *user,
    const h2_pal_timer_config_t *config,
    h2_pal_timer_t **out_timer)
{
    h2_pal_timer_t *timer;
    if (config == NULL || out_timer == NULL || config->cb == NULL || config->period_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_timer = NULL;
    timer = (h2_pal_timer_t *)h2_jieli_sdk_malloc(sizeof(*timer));
    if (timer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(timer, 0, sizeof(*timer));
    timer->config = *config;
    if ((config->flags & H2_PAL_TIMER_FLAG_AUTO_START) != 0u) {
        const h2_pal_result_t rc = timer_start(user, timer);
        if (rc != H2_PAL_OK) {
            h2_jieli_sdk_free(timer);
            return rc;
        }
    }
    *out_timer = timer;
    return H2_PAL_OK;
}

static h2_pal_result_t timer_destroy(void *user, h2_pal_timer_t *timer)
{
    const void *owner;
    if (timer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!timer->owned) {
        /* Never registered with the SDK: no fire can be queued anywhere. */
        timer_release(timer);
        return H2_PAL_OK;
    }
    owner = timer->owner_task;
    if (owner != h2_jieli_sdk_task_current()) {
        /* Another task cannot order the reclaim behind the fires queued to
         * the owner task; fail instead of freeing storage they still read. */
        return H2_PAL_ERR_INVALID_STATE;
    }
    (void)timer_stop(user, timer);
    if (h2_jieli_sdk_timer_add(timer, timer_reclaim, TIMER_RECLAIM_DELAY_MS, 0) == 0u) {
        /* Out of SDK timeout slots: a fire may already be queued to this
         * task, so the timer stays alive and owned by the caller, who can
         * retry destroy() later. It is stopped and queues no new fires. */
        return H2_PAL_ERR_UNAVAILABLE;
    }
    /* Storage is handed to the reclaim timeout; see timer_reclaim(). */
    h2_jieli_atomic_store_u32(&timer->destroyed, 1u);
    return H2_PAL_OK;
}

static h2_pal_result_t timer_reset(void *user, h2_pal_timer_t *timer)
{
    h2_pal_result_t rc;
    if (timer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = timer_stop(user, timer);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return timer_start(user, timer);
}

static h2_pal_result_t timer_set_period_ms(void *user, h2_pal_timer_t *timer, uint32_t period_ms)
{
    if (timer == NULL || period_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    timer->config.period_ms = period_ms;
    if (timer->running) {
        return timer_reset(user, timer);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t timer_is_running(void *user, h2_pal_timer_t *timer, int *out_running)
{
    (void)user;
    if (timer == NULL || out_running == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_running = timer->running ? 1 : 0;
    return H2_PAL_OK;
}

static const h2_pal_timer_vtable_t s_timer_vtable = {
    .create = timer_create,
    .destroy = timer_destroy,
    .start = timer_start,
    .stop = timer_stop,
    .reset = timer_reset,
    .set_period_ms = timer_set_period_ms,
    .is_running = timer_is_running,
};

static const h2_pal_timer_api_t s_timer_api = {
    .user = NULL,
    .vtable = &s_timer_vtable,
};

const h2_pal_timer_api_t *h2_jieli_br23_platform_timer_api(void)
{
    return &s_timer_api;
}
