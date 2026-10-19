#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include "h2_atomic.h"
#include <stdlib.h>
#include <sched.h>
#include "h2/pal/core/h2_pal_errors.h"
typedef unsigned char u8;
typedef uintptr_t u32;
typedef pthread_mutex_t OS_MUTEX;
enum { OS_NO_ERR = 0, UART_RECV_TIMEOUT = -2, UART_CIRCULAR_BUFFER_WRITE_OVERLAY = -1,
    UART_SET_CIRCULAR_BUFF_ADDR = 1, UART_SET_CIRCULAR_BUFF_LENTH,
    UART_SET_RECV_BLOCK, UART_START, UART_FLUSH };
static struct { uint16_t CON0; } uart_registers;
#define JL_UART1 (&uart_registers)
#define BIT(n) (1u << (n))
static h2_atomic_int_t held, block_lock, worker_entered, worker_release;
static int writes, reads, handle, short_write, fail_write;
static uint8_t received[1024];
static size_t received_count;
static uint32_t now, completion_at;
static const uint8_t *dma_buffer;
static size_t dma_count;
static uint8_t dma_expected[512];
static void progress(void) {
    if (dma_buffer != NULL && now >= completion_at) {
        assert(memcmp(dma_buffer, dma_expected, dma_count) == 0);
        memcpy(received + received_count, dma_buffer, dma_count);
        received_count += dma_count;
        dma_buffer = NULL;
        uart_registers.CON0 |= BIT(15);
    }
}
uint32_t timer_get_ms(void) { ++now; progress(); return now; }
void os_time_dly(unsigned ticks) { now += ticks * 10u; progress(); }
int os_mutex_create(OS_MUTEX *mutex) { return pthread_mutex_init(mutex, NULL); }
int os_mutex_accept(OS_MUTEX *mutex) {
    if (h2_atomic_load(&block_lock) || pthread_mutex_trylock(mutex) != 0) return 1;
    h2_atomic_store(&held, 1);
    return 0;
}
int os_mutex_pend(OS_MUTEX *mutex, int ticks) {
    if (h2_atomic_load(&block_lock) || h2_atomic_load(&held)) {
        now += ticks > 0 ? (uint32_t)ticks * 10u : 1000u;
        return 1;
    }
    return os_mutex_accept(mutex);
}
int os_mutex_post(OS_MUTEX *mutex) { assert(h2_atomic_load(&held)); h2_atomic_store(&held, 0); return pthread_mutex_unlock(mutex); }
void *dev_open(const char *name, void *arg) {
    (void)arg;
    assert(strcmp(name, "uart1") == 0);
    return &handle;
}
int dev_close(void *device) { assert(device == &handle); return 0; }
int dev_ioctl(void *device, int command, u32 argument) {
    (void)command;
    (void)argument;
    assert(device == &handle);
    return 0;
}
int dev_read(void *device, void *buffer, u32 size) {
    (void)buffer;
    (void)size;
    assert(device == &handle);
    ++reads;
    return 0;
}
int dev_write(void *device, void *buffer, u32 size) {
    assert(device == &handle && h2_atomic_load(&held) && size <= 512);
    assert(dma_buffer == NULL);
    ++writes;
    if (fail_write) return 0;
    if (short_write && size > 17u) size = 17u;
    if (!/* ASYNC_MODE */) {
        now += 100;
        memcpy(received + received_count, buffer, size);
        received_count += size;
        return (int)size;
    }
    dma_buffer = buffer;
    dma_count = size;
    memcpy(dma_expected, buffer, size);
    uart_registers.CON0 &= ~BIT(15);
    completion_at = now + 20;
    return (int)size;
}
/* REAL_PROVIDER */
static void *hold_writer(void *unused) {
    (void)unused;
    assert(pthread_mutex_lock(&tx_mutex) == 0);
    h2_atomic_store(&held, 1);
    h2_atomic_store(&worker_entered, 1);
    while (!h2_atomic_load(&worker_release)) sched_yield();
    assert(os_mutex_post(&tx_mutex) == 0);
    return NULL;
}
static void h2_fixture_atomic_cleanup(void) {
    h2_atomic_destroy(&held);
    h2_atomic_destroy(&block_lock);
    h2_atomic_destroy(&worker_entered);
    h2_atomic_destroy(&worker_release);
}
int main(int argc, char **argv) {
    assert(atexit(h2_fixture_atomic_cleanup) == 0);
    assert(h2_atomic_init(&held, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&block_lock, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&worker_entered, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&worker_release, 0) == H2_ATOMIC_OK);

    assert(argc == 2);
    assert(h2_jieli_ac791n_devkit_console_start() == H2_PAL_OK);
    uint8_t data[1024];
    memset(data, 0x35, sizeof(data));
    if (strcmp(argv[1], "threaded_read") == 0) {
        pthread_t worker;
        assert(pthread_create(&worker, NULL, hold_writer, NULL) == 0);
        while (!h2_atomic_load(&worker_entered)) sched_yield();
        const uint32_t before = now;
        assert(h2_jieli_ac791n_devkit_console_read(data, 1) == 0);
        assert(now == before && reads == 0);
        h2_atomic_store(&worker_release, 1);
        assert(pthread_join(worker, NULL) == 0);
    } else if (strcmp(argv[1], "read_busy") == 0) {
        h2_atomic_store(&block_lock, 1);
        const uint32_t before = now;
        assert(h2_jieli_ac791n_devkit_console_read(data, 1) == 0);
        assert(now == before && reads == 0);
    } else if (strcmp(argv[1], "zero") == 0) {
        h2_atomic_store(&block_lock, 1);
        const uint32_t before = now;
        assert(h2_jieli_ac791n_devkit_console_write(data, 1, 0) == H2_PAL_ERR_TIMEOUT);
        assert(now - before <= 2 && writes == 0);
    } else if (strcmp(argv[1], "deadline") == 0) {
        const uint32_t before = now;
        const int written = h2_jieli_ac791n_devkit_console_write(data, sizeof(data), 5);
        assert(written == 512 && now - before <= 7 && writes == 1);
        assert(!h2_atomic_load(&held) && dma_buffer != NULL);
    } else if (strcmp(argv[1], "short_write") == 0) {
        short_write = 1;
        assert(h2_jieli_ac791n_devkit_console_write(data, 50, 1000) == 50);
        assert(writes == 3 && received_count == 50 && memcmp(received, data, 50) == 0);
        fail_write = 1;
        assert(h2_jieli_ac791n_devkit_console_write(data, 1, 100) == H2_PAL_ERR_IO);
        assert(!h2_atomic_load(&held));
    } else if (strcmp(argv[1], "stalled_dma") == 0) {
        assert(h2_jieli_ac791n_devkit_console_write(data, 128, 0) == 128);
        completion_at = UINT32_MAX;
        memset(data, 0x71, sizeof(data));
        const uint32_t before = now;
        assert(h2_jieli_ac791n_devkit_console_write(data, 128, 5) == H2_PAL_ERR_TIMEOUT);
        assert(now - before <= 7 && writes == 1 && !h2_atomic_load(&held));
        assert(dma_buffer != NULL && memcmp(dma_buffer, dma_expected, dma_count) == 0);
    } else {
        assert(strcmp(argv[1], "dma_lifetime") == 0);
        assert(h2_jieli_ac791n_devkit_console_write(data, 128, 0) == 128);
        memset(data, 0x71, sizeof(data));
        const int count = h2_jieli_ac791n_devkit_console_write(data, 128, 0);
        assert(count == H2_PAL_ERR_TIMEOUT && writes == 1);
        now = completion_at;
        progress();
        assert(h2_jieli_ac791n_devkit_console_write(data, 128, 100) == 128);
        assert(writes == 2 && dma_buffer == NULL && !h2_atomic_load(&held));
    }
    return 0;
}
