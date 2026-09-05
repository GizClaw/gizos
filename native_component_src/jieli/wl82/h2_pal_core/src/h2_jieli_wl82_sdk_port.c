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
    ticks = (ms + H2_JIELI_WL82_TICK_MS - 1u) / H2_JIELI_WL82_TICK_MS;
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
    h2_jieli_sdk_sem_t *sem = (h2_jieli_sdk_sem_t *)malloc(sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }
    if (os_sem_create(&sem->native, (int)initial_count) != OS_NO_ERR) {
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
    return os_sem_post(&sem->native) == OS_NO_ERR ? 0 : -1;
}

/* ---- Tasks --------------------------------------------------------------- */

int h2_jieli_sdk_task_create(void (*entry)(void *ctx), void *ctx, const char *name, size_t stack_bytes)
{
    if (entry == NULL || name == NULL) {
        return -1;
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
