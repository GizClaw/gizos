#include "h2_jieli_wl82_sdk_port_fake.h"
#include "h2/pal/core/h2_pal_errors.h"

#include <stdlib.h>
#include <string.h>

struct h2_jieli_sdk_mutex {
    int locked;
};

struct h2_jieli_sdk_sem {
    uint32_t count;
};

typedef struct fake_timer {
    uint16_t id;
    void *ctx;
    void (*callback)(void *ctx);
    uint32_t period_ms;
    uint32_t deadline_ms;
    int repeat;
    int active;
} fake_timer_t;

static char s_log[H2_JIELI_FAKE_LOG_CAPACITY];
static size_t s_log_length;
static uint32_t s_now_ms;
static uint64_t s_clock_us;
static uint32_t s_sleep_total_ms;
static int s_live_allocations;
static fake_timer_t s_timers[H2_JIELI_FAKE_TIMER_CAPACITY];
static uint16_t s_next_timer_id = 1u;
static int s_task_create_calls;
static char s_last_task_name[32];
static size_t s_last_task_stack_bytes;
static void (*s_last_task_entry)(void *ctx);
static void *s_last_task_ctx;
static int s_fail_task_create;
static int s_fail_next_mutex_lock;
static int s_invalid_mutex_unlocks;
static int s_task_running;
static int s_task_delete_calls;

static void (*s_timer_dispatch_hook)(void);
static int s_default_task;
static const void *s_current_task = &s_default_task;

void h2_jieli_fake_reset(void)
{
    s_timer_dispatch_hook = NULL;
    s_current_task = &s_default_task;
    memset(s_log, 0, sizeof(s_log));
    s_log_length = 0u;
    s_now_ms = 0u;
    s_clock_us = 0u;
    s_sleep_total_ms = 0u;
    s_live_allocations = 0;
    memset(s_timers, 0, sizeof(s_timers));
    s_next_timer_id = 1u;
    s_task_create_calls = 0;
    memset(s_last_task_name, 0, sizeof(s_last_task_name));
    s_last_task_stack_bytes = 0u;
    s_last_task_entry = NULL;
    s_last_task_ctx = NULL;
    s_fail_task_create = 0;
    s_fail_next_mutex_lock = 0;
    s_invalid_mutex_unlocks = 0;
    s_task_running = 0;
    s_task_delete_calls = 0;
}

const char *h2_jieli_fake_log_output(void)
{
    return s_log;
}

size_t h2_jieli_fake_log_length(void)
{
    return s_log_length;
}

void h2_jieli_fake_advance_ms(uint32_t ms)
{
    s_now_ms += ms;
    s_clock_us += (uint64_t)ms * 1000u;
}

void h2_jieli_fake_set_time_ms(uint32_t ms)
{
    uint64_t epoch = (s_clock_us / 1000u) & ~UINT64_C(0xffffffff);
    if (ms < s_now_ms) epoch += UINT64_C(0x100000000);
    s_clock_us = (epoch + ms) * 1000u;
    s_now_ms = ms;
}

void h2_jieli_fake_set_time_us(uint64_t us)
{
    s_clock_us = us;
    s_now_ms = (uint32_t)(us / 1000u);
}

uint32_t h2_jieli_fake_sleep_total_ms(void)
{
    return s_sleep_total_ms;
}

int h2_jieli_fake_live_allocations(void)
{
    return s_live_allocations;
}

void *h2_jieli_sdk_malloc(size_t size)
{
    void *memory = malloc(size);
    if (memory != NULL) {
        s_live_allocations++;
    }
    return memory;
}

void *h2_jieli_sdk_realloc(void *ptr, size_t size)
{
    void *memory = realloc(ptr, size);
    if (memory != NULL && ptr == NULL) {
        s_live_allocations++;
    }
    return memory;
}

void h2_jieli_sdk_free(void *ptr)
{
    if (ptr != NULL) {
        s_live_allocations--;
        free(ptr);
    }
}

void h2_jieli_sdk_debug_write(const char *data, size_t length)
{
    size_t i;
    for (i = 0u; i < length && s_log_length + 1u < sizeof(s_log); ++i) {
        s_log[s_log_length++] = data[i];
    }
    s_log[s_log_length] = '\0';
}

uint32_t h2_jieli_sdk_time_ms(void)
{
    return s_now_ms;
}

int h2_jieli_sdk_time_us(uint64_t *out_us)
{
    if (out_us == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_us = s_clock_us;
    return H2_PAL_OK;
}

void h2_jieli_sdk_sleep_ms(uint32_t ms)
{
    s_sleep_total_ms += ms;
    h2_jieli_fake_advance_ms(ms);
}

uint32_t h2_jieli_sdk_tick_ms(void)
{
    return 10u;
}

h2_jieli_sdk_mutex_t *h2_jieli_sdk_mutex_create(void)
{
    h2_jieli_sdk_mutex_t *mutex = (h2_jieli_sdk_mutex_t *)h2_jieli_sdk_malloc(sizeof(*mutex));
    if (mutex != NULL) {
        mutex->locked = 0;
    }
    return mutex;
}

void h2_jieli_sdk_mutex_destroy(h2_jieli_sdk_mutex_t *mutex)
{
    h2_jieli_sdk_free(mutex);
}

void h2_jieli_fake_fail_next_mutex_lock(void)
{
    s_fail_next_mutex_lock = 1;
}

int h2_jieli_sdk_mutex_lock(h2_jieli_sdk_mutex_t *mutex, uint32_t timeout_ms)
{
    if (mutex == NULL) {
        return -1;
    }
    if (s_fail_next_mutex_lock) {
        s_fail_next_mutex_lock = 0;
        return -1;
    }
    if (mutex->locked) {
        /* Single-threaded fake: a contended lock can only time out. */
        if (timeout_ms != 0u && timeout_ms != H2_JIELI_SDK_WAIT_FOREVER) {
            s_now_ms += timeout_ms;
        }
        return 1;
    }
    mutex->locked = 1;
    return 0;
}

int h2_jieli_fake_invalid_mutex_unlocks(void)
{
    return s_invalid_mutex_unlocks;
}

int h2_jieli_sdk_mutex_unlock(h2_jieli_sdk_mutex_t *mutex)
{
    if (mutex == NULL || !mutex->locked) {
        ++s_invalid_mutex_unlocks;
        return -1;
    }
    mutex->locked = 0;
    return 0;
}

h2_jieli_sdk_sem_t *h2_jieli_sdk_sem_create(uint32_t initial_count)
{
    h2_jieli_sdk_sem_t *sem = (h2_jieli_sdk_sem_t *)h2_jieli_sdk_malloc(sizeof(*sem));
    if (sem != NULL) {
        sem->count = initial_count;
    }
    return sem;
}

void h2_jieli_sdk_sem_destroy(h2_jieli_sdk_sem_t *sem)
{
    h2_jieli_sdk_free(sem);
}

static void (*s_sem_timeout_hook)(void);
void h2_jieli_fake_set_sem_timeout_hook(void (*hook)(void))
{
    s_sem_timeout_hook = hook;
}

int h2_jieli_sdk_sem_take(h2_jieli_sdk_sem_t *sem, uint32_t timeout_ms)
{
    if (sem == NULL) {
        return -1;
    }
    if (sem->count > 0u) {
        sem->count--;
        return 0;
    }
    if (timeout_ms != 0u && timeout_ms != H2_JIELI_SDK_WAIT_FOREVER) {
        s_now_ms += timeout_ms;
        if (s_sem_timeout_hook != NULL) {
            void (*hook)(void) = s_sem_timeout_hook;
            s_sem_timeout_hook = NULL;
            hook();
        }
    }
    return 1;
}

int h2_jieli_sdk_sem_give(h2_jieli_sdk_sem_t *sem)
{
    if (sem == NULL) {
        return -1;
    }
    sem->count++;
    return 0;
}

int h2_jieli_sdk_task_create(void (*entry)(void *ctx), void *ctx, const char *name, size_t stack_bytes)
{
    s_task_create_calls++;
    if (s_fail_task_create || entry == NULL || name == NULL) {
        return -1;
    }
    strncpy(s_last_task_name, name, sizeof(s_last_task_name) - 1u);
    s_last_task_stack_bytes = stack_bytes;
    s_last_task_entry = entry;
    s_last_task_ctx = ctx;
    return 0;
}

int h2_jieli_sdk_task_delete(const char *name)
{
    if (name == NULL || name[0] == '\0' || s_task_running) {
        return -1;
    }
    s_task_delete_calls++;
    return 0;
}

void h2_jieli_sdk_task_park(void)
{
    /* The fake runs task entries synchronously; return to the test. */
    s_task_running = 0;
}

int h2_jieli_fake_task_create_calls(void)
{
    return s_task_create_calls;
}

const char *h2_jieli_fake_last_task_name(void)
{
    return s_last_task_name;
}

size_t h2_jieli_fake_last_task_stack_bytes(void)
{
    return s_last_task_stack_bytes;
}

void h2_jieli_fake_run_last_task_once(void)
{
    if (s_last_task_entry != NULL) {
        s_task_running = 1;
        s_last_task_entry(s_last_task_ctx);
    }
}

void h2_jieli_fake_fail_task_create(int fail)
{
    s_fail_task_create = fail;
}

uint16_t h2_jieli_sdk_timer_add(void *ctx, void (*callback)(void *ctx), uint32_t period_ms, int repeat)
{
    size_t i;
    for (i = 0u; i < H2_JIELI_FAKE_TIMER_CAPACITY; ++i) {
        if (!s_timers[i].active) {
            s_timers[i].id = s_next_timer_id++;
            s_timers[i].ctx = ctx;
            s_timers[i].callback = callback;
            s_timers[i].period_ms = period_ms;
            s_timers[i].deadline_ms = s_now_ms + period_ms;
            s_timers[i].repeat = repeat;
            s_timers[i].active = 1;
            return s_timers[i].id;
        }
    }
    return 0u;
}

void h2_jieli_sdk_timer_del(uint16_t id, int repeat)
{
    size_t i;
    (void)repeat;
    for (i = 0u; i < H2_JIELI_FAKE_TIMER_CAPACITY; ++i) {
        if (s_timers[i].active && s_timers[i].id == id) {
            s_timers[i].active = 0;
        }
    }
}

void h2_jieli_fake_set_timer_dispatch_hook(void (*hook)(void))
{
    s_timer_dispatch_hook = hook;
}

void h2_jieli_fake_set_current_task(const void *handle)
{
    s_current_task = handle != NULL ? handle : (const void *)&s_default_task;
}

const void *h2_jieli_sdk_task_current(void)
{
    return s_current_task;
}

void h2_jieli_fake_run_timers(void)
{
    size_t i;
    for (i = 0u; i < H2_JIELI_FAKE_TIMER_CAPACITY; ++i) {
        fake_timer_t *timer = &s_timers[i];
        if (!timer->active || (int32_t)(s_now_ms - timer->deadline_ms) < 0) {
            continue;
        }
        /* Capture the dispatch first: the hook or the callback may delete
         * this timer and register another one that reuses the slot. */
        void (*callback)(void *ctx) = timer->callback;
        void *ctx = timer->ctx;
        if (timer->repeat) {
            timer->deadline_ms += timer->period_ms;
        } else {
            timer->active = 0;
        }
        if (s_timer_dispatch_hook != NULL) {
            void (*hook)(void) = s_timer_dispatch_hook;
            s_timer_dispatch_hook = NULL;
            hook();
        }
        callback(ctx);
    }
}

size_t h2_jieli_fake_timer_count(void)
{
    size_t i;
    size_t count = 0u;
    for (i = 0u; i < H2_JIELI_FAKE_TIMER_CAPACITY; ++i) {
        if (s_timers[i].active) {
            count++;
        }
    }
    return count;
}
