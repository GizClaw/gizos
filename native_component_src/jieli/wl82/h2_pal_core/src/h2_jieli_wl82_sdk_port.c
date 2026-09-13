/**
 * @file h2_jieli_wl82_sdk_port.c
 * @brief JieLi AC791N (wl82) SDK binding for the PAL core providers.
 *
 * This translation unit is the only place in the component that includes SDK
 * headers. It is compiled by the SDK native build (jieli_firmware) with the
 * SDK include roots; host tests link the fake in tests/ instead.
 */

#include "h2_jieli_wl82_sdk_port.h"
#include "h2_jieli_wl82_platform_core.h"

#include "system/includes.h"
#include "system/timer.h"

#include <string.h>

#ifndef H2_JIELI_WL82_TICK_MS
/* wl82 FreeRTOSConfig: configTICK_RATE_HZ = 100. */
#define H2_JIELI_WL82_TICK_MS 10u
#endif

struct h2_jieli_sdk_mutex {
    OS_MUTEX native;
};

struct h2_jieli_sdk_sem {
    OS_SEM native;
};

/* ---- Memory --------------------------------------------------------------
 * The SDK heap exports malloc/free but no realloc, so every block carries a
 * small aligned header recording its size for realloc copies. */

#define H2_JIELI_WL82_MEM_HEADER 16u

static size_t *mem_header(void *ptr)
{
    return (size_t *)((uint8_t *)ptr - H2_JIELI_WL82_MEM_HEADER);
}

void *h2_jieli_sdk_malloc(size_t size)
{
    uint8_t *block;
    if (size == 0u || size > SIZE_MAX - H2_JIELI_WL82_MEM_HEADER) {
        return NULL;
    }
    block = (uint8_t *)malloc(size + H2_JIELI_WL82_MEM_HEADER);
    if (block == NULL) {
        return NULL;
    }
    *(size_t *)block = size;
    return block + H2_JIELI_WL82_MEM_HEADER;
}

void *h2_jieli_sdk_realloc(void *ptr, size_t size)
{
    void *replacement;
    size_t previous;
    if (ptr == NULL) {
        return h2_jieli_sdk_malloc(size);
    }
    if (size == 0u) {
        h2_jieli_sdk_free(ptr);
        return NULL;
    }
    previous = *mem_header(ptr);
    if (previous >= size) {
        return ptr;
    }
    replacement = h2_jieli_sdk_malloc(size);
    if (replacement == NULL) {
        return NULL;
    }
    memcpy(replacement, ptr, previous);
    h2_jieli_sdk_free(ptr);
    return replacement;
}

void h2_jieli_sdk_free(void *ptr)
{
    if (ptr != NULL) {
        free(mem_header(ptr));
    }
}

/* ---- Layout-selected debug output ---------------------------------------- */

void h2_jieli_sdk_debug_write(const char *data, size_t length)
{
    if (data == NULL || length == 0u) {
        return;
    }
    /* This is text, not an SDK buffer dump. Copy the length-delimited input
     * before using %s: callers need not provide a trailing NUL. A PAL log
     * line fits in one call, retaining the SDK's normal buffered producer
     * and the layout-selected UART/USB drain and protocol write lock. */
    char text[H2_JIELI_WL82_LOG_LINE_MAX + 1u];
    while (length != 0u) {
        size_t take = length < sizeof(text) - 1u ? length : sizeof(text) - 1u;
        memcpy(text, data, take);
        text[take] = '\0';
        (void)printf("%s", text);
        data += take;
        length -= take;
    }
}

/* ---- Time ---------------------------------------------------------------- */

uint32_t h2_jieli_sdk_time_ms(void)
{
    return (uint32_t)timer_get_ms();
}

extern int h2_jieli_ac791n_devkit_clock_read_us(uint64_t *out_us) __attribute__((weak));

int h2_jieli_sdk_time_us(uint64_t *out_us)
{
    if (out_us == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (h2_jieli_ac791n_devkit_clock_read_us == NULL) return H2_PAL_ERR_UNSUPPORTED;
    return h2_jieli_ac791n_devkit_clock_read_us(out_us);
}

uint32_t h2_jieli_sdk_tick_ms(void)
{
    return H2_JIELI_WL82_TICK_MS;
}

static int ms_to_ticks(uint32_t ms)
{
    uint32_t ticks;
    if (ms == 0u) {
        return 0;
    }
    ticks = ms / H2_JIELI_WL82_TICK_MS +
            (ms % H2_JIELI_WL82_TICK_MS != 0u);
    if (ticks == 0u) {
        ticks = 1u;
    }
    if (ticks > 0x7fffffffu) {
        ticks = 0x7fffffffu;
    }
    return (int)ticks;
}

void h2_jieli_sdk_sleep_ms(uint32_t ms)
{
    int ticks = ms_to_ticks(ms);
    if (ticks == 0) {
        ticks = 1;
    }
    os_time_dly(ticks);
}

/* ---- Synchronization -----------------------------------------------------
 * os_api pend timeouts are in ticks and 0 means "wait forever"; PAL timeouts
 * are milliseconds with 0 meaning "do not wait". The pinned AC791N system.a
 * exports os_sem_accept/os_mutex_accept; use these documented non-blocking
 * operations rather than depending on private FreeRTOS semaphore storage. */

static int map_os_wait(int rc)
{
    if (rc == OS_NO_ERR) {
        return 0;
    }
    return rc == OS_TIMEOUT ? 1 : -1;
}

h2_jieli_sdk_mutex_t *h2_jieli_sdk_mutex_create(void)
{
    h2_jieli_sdk_mutex_t *mutex = (h2_jieli_sdk_mutex_t *)malloc(sizeof(*mutex));
    if (mutex == NULL) {
        return NULL;
    }
    if (os_mutex_create(&mutex->native) != OS_NO_ERR) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

void h2_jieli_sdk_mutex_destroy(h2_jieli_sdk_mutex_t *mutex)
{
    if (mutex == NULL) {
        return;
    }
    (void)os_mutex_del(&mutex->native, 0);
    free(mutex);
}

int h2_jieli_sdk_mutex_lock(h2_jieli_sdk_mutex_t *mutex, uint32_t timeout_ms)
{
    if (mutex == NULL) {
        return -1;
    }
    if (timeout_ms == 0u) {
        return map_os_wait(os_mutex_accept(&mutex->native));
    }
    if (timeout_ms == H2_JIELI_SDK_WAIT_FOREVER) {
        return map_os_wait(os_mutex_pend(&mutex->native, 0));
    }
    return map_os_wait(os_mutex_pend(&mutex->native, ms_to_ticks(timeout_ms)));
}

int h2_jieli_sdk_mutex_unlock(h2_jieli_sdk_mutex_t *mutex)
{
    if (mutex == NULL) {
        return -1;
    }
    return os_mutex_post(&mutex->native) == OS_NO_ERR ? 0 : -1;
}

h2_jieli_sdk_sem_t *h2_jieli_sdk_sem_create(uint32_t initial_count)
{
    return h2_jieli_sdk_sem_create_bounded(initial_count, UINT32_MAX);
}

h2_jieli_sdk_sem_t *h2_jieli_sdk_sem_create_bounded(uint32_t initial_count, uint32_t max_count)
{
    if (max_count == 0u || initial_count > max_count) return NULL;
    h2_jieli_sdk_sem_t *sem = (h2_jieli_sdk_sem_t *)malloc(sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }
    if (xQueueCreateCountingSemaphoreStatic(max_count, initial_count, &sem->native) == NULL) {
        free(sem);
        return NULL;
    }
    return sem;
}

void h2_jieli_sdk_sem_destroy(h2_jieli_sdk_sem_t *sem)
{
    if (sem == NULL) {
        return;
    }
    (void)os_sem_del(&sem->native, 0);
    free(sem);
}

int h2_jieli_sdk_sem_take(h2_jieli_sdk_sem_t *sem, uint32_t timeout_ms)
{
    if (sem == NULL) {
        return -1;
    }
    if (timeout_ms == 0u) {
        return map_os_wait(os_sem_accept(&sem->native));
    }
    if (timeout_ms == H2_JIELI_SDK_WAIT_FOREVER) {
        return map_os_wait(os_sem_pend(&sem->native, 0));
    }
    return map_os_wait(os_sem_pend(&sem->native, ms_to_ticks(timeout_ms)));
}

int h2_jieli_sdk_sem_give(h2_jieli_sdk_sem_t *sem)
{
    if (sem == NULL) {
        return -1;
    }
    /* The pinned SDK returns nonzero only when the native queue is full;
     * retain its task/ISR dispatch and yield handling. */
    return os_sem_post(&sem->native) == OS_NO_ERR ? 0 : 1;
}

/* ---- Tasks --------------------------------------------------------------- */

/* Generated by the firmware target's Bazel task-policy component. Older
 * targets without a default remain fail-closed for dynamic task names. */
extern const struct task_info h2_jieli_default_task_policy
    __attribute__((weak));

int h2_jieli_sdk_task_create(void (*entry)(void *ctx), void *ctx, const char *name, size_t stack_bytes)
{
    if (entry == NULL || name == NULL) {
        return -1;
    }
    if (strncmp(name, "$h2anon/", 8u) == 0) {
        const struct task_info *policy = &h2_jieli_default_task_policy;
        if (policy == NULL || policy->stack_size == 0u ||
            stack_bytes > UINT32_MAX - 3u) {
            return -1;
        }
        u32 stack_words = (u32)((stack_bytes + 3u) / 4u);
        if (stack_words < policy->stack_size) stack_words = policy->stack_size;
        return os_task_create(entry, ctx, policy->prio, stack_words,
                              policy->qsize, name) == OS_NO_ERR ? 0 : -1;
    }
    /* JieLi requires every task to be registered in the target's
     * task_info_table.  Using os_task_create() here bypassed that policy and
     * forced every portable task to priority 2, so BT controller/stack tasks
     * starved both Loader command transports once BLE was enabled. */
    (void)stack_bytes;
    return task_create(entry, ctx, name) == OS_NO_ERR ? 0 : -1;
}

int h2_jieli_sdk_task_delete(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    return os_task_del(name) == OS_NO_ERR ? 0 : -1;
}

void h2_jieli_sdk_task_park(void)
{
    for (;;) {
        os_time_dly(100);
    }
}

const void *h2_jieli_sdk_task_current(void)
{
    return (const void *)xTaskGetCurrentTaskHandle();
}

/* ---- Timers -------------------------------------------------------------- */

uint16_t h2_jieli_sdk_timer_add(void *ctx, void (*callback)(void *ctx), uint32_t period_ms, int repeat)
{
    if (callback == NULL || period_ms == 0u) {
        return 0u;
    }
    if (repeat) {
        return sys_timer_add(ctx, callback, period_ms);
    }
    return sys_timeout_add(ctx, callback, period_ms);
}

void h2_jieli_sdk_timer_del(uint16_t id, int repeat)
{
    if (id == 0u) {
        return;
    }
    if (repeat) {
        sys_timer_del(id);
    } else {
        sys_timeout_del(id);
    }
}

/* ---------------------------------------------------------------------------
 * Atomic runtime for C11/GCC atomics on the dual-core wl82.
 *
 * pi32v2 clang lowers every atomic read-modify-write to __sync_* libcalls.
 * The toolchain's compiler-rt versions bracket the operation with a bare
 * lockset/lockclr pair; the SDK retired that pattern for cross-core locking
 * (asm/cpu.h keeps it under #if 0, it needs a per-CPU nesting count) and
 * uses testset spinlocks instead. Under concurrent use from both cores the
 * compiler-rt operations lose updates: the PAL system-event lifecycle word
 * dropped from ACTIVE to ACTIVE-1 and rejected every later subscription.
 *
 * These strong definitions take precedence over the compiler-rt archive
 * members for the whole image, so PAL, board, portable libraries and SDK
 * code all share one testset-guarded implementation. `used` keeps them
 * through LTO, where the libcalls only appear after code generation.
 * ------------------------------------------------------------------------- */

static spinlock_t h2_jieli_atomic_lock = {.rwlock = 0};

/* clang reserves the __sync_*_N spellings as builtins; define ordinary
 * functions and bind them to the libcall symbols with asm labels. */
#define H2_JIELI_SYNC_RMW(width, type, name, update)                         \
    __attribute__((used)) type h2_jieli_sync_##name##_##width(             \
        volatile type *address, type value)                                \
        __asm__("__sync_" #name "_" #width);                               \
    type h2_jieli_sync_##name##_##width(                                   \
        volatile type *address, type value) {                              \
        spin_lock(&h2_jieli_atomic_lock);                                  \
        const type previous = *address;                                    \
        *address = (update);                                               \
        spin_unlock(&h2_jieli_atomic_lock);                                \
        return previous;                                                   \
    }

#define H2_JIELI_SYNC_CAS(width, type)                                       \
    __attribute__((used)) type h2_jieli_sync_cas_##width(                  \
        volatile type *address, type expected, type desired)               \
        __asm__("__sync_val_compare_and_swap_" #width);                    \
    type h2_jieli_sync_cas_##width(                                        \
        volatile type *address, type expected, type desired) {             \
        spin_lock(&h2_jieli_atomic_lock);                                  \
        const type previous = *address;                                    \
        if (previous == expected) *address = desired;                      \
        spin_unlock(&h2_jieli_atomic_lock);                                \
        return previous;                                                   \
    }

#define H2_JIELI_SYNC_WIDTH(width, type)                                     \
    H2_JIELI_SYNC_RMW(width, type, fetch_and_add, previous + value)        \
    H2_JIELI_SYNC_RMW(width, type, fetch_and_sub, previous - value)        \
    H2_JIELI_SYNC_RMW(width, type, fetch_and_and, previous & value)        \
    H2_JIELI_SYNC_RMW(width, type, fetch_and_or, previous | value)         \
    H2_JIELI_SYNC_RMW(width, type, fetch_and_xor, previous ^ value)        \
    H2_JIELI_SYNC_RMW(width, type, lock_test_and_set, value)               \
    H2_JIELI_SYNC_CAS(width, type)

H2_JIELI_SYNC_WIDTH(1, uint8_t)
H2_JIELI_SYNC_WIDTH(2, uint16_t)
H2_JIELI_SYNC_WIDTH(4, uint32_t)
H2_JIELI_SYNC_WIDTH(8, uint64_t)
