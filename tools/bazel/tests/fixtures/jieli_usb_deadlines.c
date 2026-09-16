#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include "h2/pal/core/h2_pal_errors.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned usb_dev;
typedef pthread_mutex_t OS_MUTEX;
#define OS_NO_ERR 0
#define BIT(n) (1u << (n))
#define USB_MAX_HW_NUM 1u
#define CDC_DATA_EP_IN 3u
#define MAXP_SIZE_CDC_BULKIN 64u
static OS_MUTEX usb_tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct usb_cdc_gadget { uint32_t bmTransceiver; OS_MUTEX mutex_data; } gadget = {
    .bmTransceiver = BIT(1) | BIT(4), .mutex_data = PTHREAD_MUTEX_INITIALIZER
};
static struct usb_cdc_gadget *cdc_hdl[] = {&gadget};
static struct h2_jieli_app_console { OS_MUTEX tx_mutex; usb_dev usb_id; } app = {
    .tx_mutex = PTHREAD_MUTEX_INITIALIZER
};
typedef struct h2_jieli_app_console h2_jieli_app_console_t;
static uint32_t now, first_clock, completion, transferred;
static int first_clock_set, stalled, absent_dma, short_native;
static uint8_t dma[64], received[256];
static int pending;
static atomic_int worker_entered, worker_release;
static uint32_t timer_get_ms(void) {
    ++now;
    if (!first_clock_set) { first_clock = now; first_clock_set = 1; }
    return now;
}
static void os_time_dly(unsigned ticks) { now += ticks * 10u; }
static int os_mutex_accept(OS_MUTEX *mutex) { return pthread_mutex_trylock(mutex); }
static int os_mutex_pend(OS_MUTEX *mutex, int ticks) {
    if (os_mutex_accept(mutex) == 0) return 0;
    now += ticks > 0 ? (uint32_t)ticks * 10u : 2000u;
    return 1;
}
static int os_mutex_post(OS_MUTEX *mutex) { return pthread_mutex_unlock(mutex); }
void *usb_get_dma_taddr(usb_dev id, u32 ep) {
    assert(id == 0u && ep == CDC_DATA_EP_IN);
    return absent_dma ? NULL : dma;
}
u32 usb_get_dma_size(usb_dev id, u32 ep) { (void)id; (void)ep; return sizeof(dma); }
u32 usb_read_txcsr(usb_dev id, u32 ep) {
    (void)id;
    (void)ep;
    return stalled || (pending && now < completion) ? BIT(0) : 0u;
}
u32 usb_g_bulk_write(usb_dev id, u32 ep, u8 *buffer, u32 count) {
    assert(!absent_dma);
    if (usb_read_txcsr(id, ep) != 0u) {
        now += 2000u;
        return 0u;
    }
    if (short_native && count > 17u) count = 17u;
    if (count != 0u) {
        assert(buffer != NULL && transferred + count <= sizeof(received));
        memcpy(dma, buffer, count);
        memcpy(received + transferred, dma, count);
        transferred += count;
    }
    pending = 1;
    completion = now + 1u;
    return count;
}
/* SDK_WRITE */
/* TICKS */
/* PHYSICAL_WRITE */
static void *hold_native_writer(void *unused) {
    (void)unused;
    assert(pthread_mutex_lock(&gadget.mutex_data) == 0);
    atomic_store(&worker_entered, 1);
    while (!atomic_load(&worker_release)) sched_yield();
    assert(pthread_mutex_unlock(&gadget.mutex_data) == 0);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    (void)app;
    (void)usb_tx_mutex;
    if (strcmp(argv[1], "overflow") == 0) {
        assert(ms_to_ticks(UINT32_MAX) == UINT32_MAX / 10u + 1u);
        return 0;
    }
    uint8_t bytes[64];
    memset(bytes, 0x5a, sizeof(bytes));
    uint32_t budget = 5u;
    int locked = 0, threaded = 0;
    pthread_t worker;
    OS_MUTEX *outer = /* OUTER_MUTEX */;
    void *user = /* USER */;
    if (strcmp(argv[1], "zero_lock") == 0 || strcmp(argv[1], "lock") == 0) {
        assert(pthread_mutex_lock(outer) == 0);
        locked = 1;
        if (strcmp(argv[1], "zero_lock") == 0) budget = 0u;
    } else if (strcmp(argv[1], "native_busy") == 0) {
        stalled = 1;
    } else if (strcmp(argv[1], "offline") == 0) {
        gadget.bmTransceiver = 0u;
    } else if (strcmp(argv[1], "dma_missing") == 0) {
        absent_dma = 1;
    } else if (strcmp(argv[1], "native_lock") == 0) {
        threaded = 1;
        budget = 0u;
        assert(pthread_create(&worker, NULL, hold_native_writer, NULL) == 0);
        while (!atomic_load(&worker_entered)) sched_yield();
    } else {
        budget = 100u;
        short_native = strcmp(argv[1], "short") == 0;
    }
    size_t written = 0;
    int result = physical_write(user, bytes, sizeof(bytes), &written, budget);
    if (threaded) {
        atomic_store(&worker_release, 1);
        assert(pthread_join(worker, NULL) == 0);
    }
    if (locked) assert(pthread_mutex_unlock(outer) == 0);
    assert(now - first_clock <= budget);
    if (strcmp(argv[1], "packet") == 0 || strcmp(argv[1], "short") == 0) {
        assert(result == H2_PAL_OK && written == sizeof(bytes));
        assert(transferred == sizeof(bytes) && memcmp(received, bytes, sizeof(bytes)) == 0);
    } else {
        assert(result == H2_PAL_ERR_TIMEOUT && written == 0u);
    }
    return 0;
}
