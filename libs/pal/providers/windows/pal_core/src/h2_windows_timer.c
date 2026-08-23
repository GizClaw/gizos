#include "h2_windows_internal.h"

struct h2_pal_timer {
    h2_windows_platform_t *platform;
    PTP_TIMER native;
    h2_pal_timer_cb_t callback;
    void *callback_user;
    SRWLOCK lock;
    uint32_t period_ms;
    uint32_t flags;
    int running;
};

static FILETIME windows_timer_due_time(uint32_t period_ms) {
    ULARGE_INTEGER due;
    FILETIME result;
    due.QuadPart = (uint64_t)(-(int64_t)period_ms * INT64_C(10000));
    result.dwLowDateTime = due.LowPart;
    result.dwHighDateTime = due.HighPart;
    return result;
}

static VOID CALLBACK windows_timer_callback(
    PTP_CALLBACK_INSTANCE instance,
    void *context,
    PTP_TIMER native) {
    (void)instance;
    (void)native;
    h2_pal_timer_t *timer = context;
    AcquireSRWLockExclusive(&timer->lock);
    h2_pal_timer_cb_t callback = timer->callback;
    void *callback_user = timer->callback_user;
    if ((timer->flags & H2_PAL_TIMER_FLAG_REPEAT) == 0u) {
        timer->running = 0;
    }
    ReleaseSRWLockExclusive(&timer->lock);
    callback(callback_user, timer);
}

static h2_pal_result_t windows_timer_schedule(h2_pal_timer_t *timer) {
    FILETIME due = windows_timer_due_time(timer->period_ms);
    DWORD repeat = (timer->flags & H2_PAL_TIMER_FLAG_REPEAT) != 0u
                       ? timer->period_ms
                       : 0u;
    SetThreadpoolTimer(timer->native, &due, repeat, 0u);
    timer->running = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_timer_create(
    void *user,
    const h2_pal_timer_config_t *config,
    h2_pal_timer_t **out_timer) {
    h2_windows_platform_t *platform = user;
    if (config == NULL || out_timer == NULL || config->cb == NULL ||
        config->period_ms == 0u ||
        (config->flags & ~(H2_PAL_TIMER_FLAG_REPEAT |
                           H2_PAL_TIMER_FLAG_AUTO_START)) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_timer = NULL;
    h2_pal_timer_t *timer = h2_windows_heap_alloc(sizeof(*timer));
    if (timer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    timer->platform = platform;
    timer->callback = config->cb;
    timer->callback_user = config->cb_user;
    timer->period_ms = config->period_ms;
    timer->flags = config->flags;
    InitializeSRWLock(&timer->lock);
    timer->native = CreateThreadpoolTimer(windows_timer_callback, timer, NULL);
    if (timer->native == NULL) {
        h2_windows_heap_free(timer);
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_windows_object_acquire(platform);
    if ((config->flags & H2_PAL_TIMER_FLAG_AUTO_START) != 0u) {
        (void)windows_timer_schedule(timer);
    }
    *out_timer = timer;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_timer_destroy(void *user, h2_pal_timer_t *timer) {
    h2_windows_platform_t *platform = user;
    if (timer == NULL || timer->platform != platform) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    SetThreadpoolTimer(timer->native, NULL, 0u, 0u);
    WaitForThreadpoolTimerCallbacks(timer->native, TRUE);
    CloseThreadpoolTimer(timer->native);
    h2_windows_heap_free(timer);
    h2_windows_object_release(platform);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_timer_start(void *user, h2_pal_timer_t *timer) {
    if (timer == NULL || timer->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    AcquireSRWLockExclusive(&timer->lock);
    h2_pal_result_t result = windows_timer_schedule(timer);
    ReleaseSRWLockExclusive(&timer->lock);
    return result;
}

static h2_pal_result_t windows_timer_stop(void *user, h2_pal_timer_t *timer) {
    if (timer == NULL || timer->platform != user) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    AcquireSRWLockExclusive(&timer->lock);
    SetThreadpoolTimer(timer->native, NULL, 0u, 0u);
    timer->running = 0;
    ReleaseSRWLockExclusive(&timer->lock);
    WaitForThreadpoolTimerCallbacks(timer->native, TRUE);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_timer_reset(void *user, h2_pal_timer_t *timer) {
    return windows_timer_start(user, timer);
}

static h2_pal_result_t windows_timer_set_period(
    void *user,
    h2_pal_timer_t *timer,
    uint32_t period_ms) {
    if (timer == NULL || timer->platform != user || period_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    AcquireSRWLockExclusive(&timer->lock);
    timer->period_ms = period_ms;
    if (timer->running != 0) {
        (void)windows_timer_schedule(timer);
    }
    ReleaseSRWLockExclusive(&timer->lock);
    return H2_PAL_OK;
}

static h2_pal_result_t windows_timer_is_running(
    void *user,
    h2_pal_timer_t *timer,
    int *out_running) {
    if (timer == NULL || timer->platform != user || out_running == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    AcquireSRWLockShared(&timer->lock);
    *out_running = timer->running;
    ReleaseSRWLockShared(&timer->lock);
    return H2_PAL_OK;
}

const h2_pal_timer_vtable_t h2_windows_timer_vtable = {
    .create = windows_timer_create,
    .destroy = windows_timer_destroy,
    .start = windows_timer_start,
    .stop = windows_timer_stop,
    .reset = windows_timer_reset,
    .set_period_ms = windows_timer_set_period,
    .is_running = windows_timer_is_running,
};
