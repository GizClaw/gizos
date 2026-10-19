#include "h2_bk_wifi_lease.h"

#include <stddef.h>

#if defined(H2_BK_WIFI_LEASE_TEST)
#include <pthread.h>
extern int cif_send_customer_event(uint8_t *data, uint16_t len);
static pthread_mutex_t writer_mutex = PTHREAD_MUTEX_INITIALIZER;
#else
#include <components/event.h>
#include <modules/wifi_types.h>
#include <os/os.h>
#include "cif_cntrl.h"
static beken_mutex_t writer_mutex;
static uint32_t registered;
#endif

_Static_assert(sizeof(h2_bk_wifi_lease_record_t) == 5u * sizeof(uint32_t),
               "paired lease record layout");
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0),
               "paired AP/CP lease fields require lock-free 32-bit atomics");

static uint32_t guard, generation, count, last_sequence, wake_failures, ready;
static h2_bk_wifi_lease_record_t records[H2_BK_WIFI_LEASE_CAPACITY];

#define LOAD(field) __atomic_load_n(&(field), __ATOMIC_SEQ_CST)
#define STORE(field, value) __atomic_store_n(&(field), (value), __ATOMIC_SEQ_CST)

static int h2_bk_lease_writer_lock(void) {
#if defined(H2_BK_WIFI_LEASE_TEST)
    return pthread_mutex_lock(&writer_mutex);
#else
    return writer_mutex != NULL && rtos_lock_mutex(&writer_mutex) == kNoErr ? 0 : -1;
#endif
}

static void h2_bk_lease_writer_unlock(void) {
#if defined(H2_BK_WIFI_LEASE_TEST)
    (void)pthread_mutex_unlock(&writer_mutex);
#else
    (void)rtos_unlock_mutex(&writer_mutex);
#endif
}

#if !defined(H2_BK_WIFI_LEASE_TEST)
static bk_err_t h2_bk_lease_ap_left(void *arg, event_module_t module,
                                    int event_id, void *event_data) {
    (void)arg;
    if (module == EVENT_MOD_WIFI && event_id == EVENT_WIFI_AP_DISCONNECTED &&
        event_data != NULL)
        h2_bk_wifi_lease_forget(((const wifi_event_ap_disconnected_t *)event_data)->mac);
    return BK_OK;
}
#endif

static int h2_bk_lease_prepare_writer(void) {
#if defined(H2_BK_WIFI_LEASE_TEST)
    return 0;
#else
    if (writer_mutex == NULL && rtos_init_mutex(&writer_mutex) != kNoErr)
        return -1;
    if (LOAD(registered) == 0u) {
        if (bk_event_register_cb(EVENT_MOD_WIFI, EVENT_WIFI_AP_DISCONNECTED,
                                 h2_bk_lease_ap_left, NULL) != BK_OK)
            return -1;
        STORE(registered, 1u);
    }
    return 0;
#endif
}

static uint32_t h2_bk_lease_mac_hi(const uint8_t mac[6]) {
    return ((uint32_t)mac[0] << 8u) | mac[1];
}

static uint32_t h2_bk_lease_mac_lo(const uint8_t mac[6]) {
    return ((uint32_t)mac[2] << 24u) | ((uint32_t)mac[3] << 16u) |
           ((uint32_t)mac[4] << 8u) | mac[5];
}

void h2_bk_wifi_lease_reset(void) {
    /* The SDK stops and joins its DHCP worker before restarting it. The
     * mutex also serializes a client's L2 leave callback with its ACK. */
    if (h2_bk_lease_prepare_writer() != 0 || h2_bk_lease_writer_lock() != 0) {
        STORE(ready, 0u);
        return;
    }
    uint32_t before = LOAD(guard);
    if (before & 1u) {
        h2_bk_lease_writer_unlock();
        STORE(ready, 0u);
        return;
    }
    STORE(guard, before + 1u);
    uint32_t next = LOAD(generation) + 1u;
    if (next == 0u) next = 1u;
    STORE(generation, next);
    STORE(count, 0u);
    for (unsigned i = 0u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i) {
        STORE(records[i].sequence, 0u);
        STORE(records[i].ip4, 0u);
        STORE(records[i].mac_hi, 0u);
        STORE(records[i].mac_lo, 0u);
        STORE(records[i].xid, 0u);
    }
    STORE(guard, before + 2u);
    STORE(ready, 1u);
    h2_bk_lease_writer_unlock();
}

void h2_bk_wifi_lease_forget(const uint8_t mac[6]) {
    if (mac == NULL || LOAD(ready) == 0u || h2_bk_lease_writer_lock() != 0) return;
    const uint32_t hi = h2_bk_lease_mac_hi(mac), lo = h2_bk_lease_mac_lo(mac);
    const uint32_t n = LOAD(count);
    for (uint32_t i = 0u; i < n; ++i) {
        if (LOAD(records[i].mac_hi) != hi || LOAD(records[i].mac_lo) != lo) continue;
        const uint32_t last = n - 1u;
        const uint32_t before = LOAD(guard);
        STORE(guard, before + 1u);
        if (i != last) {
            STORE(records[i].sequence, LOAD(records[last].sequence));
            STORE(records[i].ip4, LOAD(records[last].ip4));
            STORE(records[i].mac_hi, LOAD(records[last].mac_hi));
            STORE(records[i].mac_lo, LOAD(records[last].mac_lo));
            STORE(records[i].xid, LOAD(records[last].xid));
        }
        STORE(records[last].sequence, 0u);
        STORE(records[last].ip4, 0u);
        STORE(records[last].mac_hi, 0u);
        STORE(records[last].mac_lo, 0u);
        STORE(records[last].xid, 0u);
        STORE(count, last);
        STORE(guard, before + 2u);
        break;
    }
    h2_bk_lease_writer_unlock();
}

uint32_t h2_bk_wifi_lease_accept(const uint8_t mac[6], uint32_t ip4,
                                  uint32_t xid) {
    if (mac == NULL || ip4 == 0u || (mac[0] & 1u) != 0u ||
        LOAD(ready) == 0u) return 0u;
    const uint32_t hi = h2_bk_lease_mac_hi(mac), lo = h2_bk_lease_mac_lo(mac);
    if ((hi | lo) == 0u) return 0u;

    if (h2_bk_lease_writer_lock() != 0) return 0u;
    uint32_t before = LOAD(guard);
    if ((before & 1u) != 0u || LOAD(generation) == 0u) {
        h2_bk_lease_writer_unlock();
        return 0u;
    }
    STORE(guard, before + 1u);
    uint32_t n = LOAD(count), slot = n;
    for (uint32_t i = 0u; i < n; ++i) {
        if (LOAD(records[i].mac_hi) == hi && LOAD(records[i].mac_lo) == lo) {
            slot = i;
            break;
        }
    }
    if (slot == H2_BK_WIFI_LEASE_CAPACITY) {
        STORE(guard, before + 2u);
        h2_bk_lease_writer_unlock();
        return 0u;
    }
    if (slot == n) STORE(count, n + 1u);
    uint32_t sequence = LOAD(records[slot].sequence);
    if (sequence == 0u || LOAD(records[slot].xid) != xid ||
        LOAD(records[slot].ip4) != ip4) {
        sequence = LOAD(last_sequence) + 1u;
        if (sequence == 0u) sequence = 1u;
        STORE(last_sequence, sequence);
    }
    STORE(records[slot].ip4, ip4);
    STORE(records[slot].mac_hi, hi);
    STORE(records[slot].mac_lo, lo);
    STORE(records[slot].xid, xid);
    STORE(records[slot].sequence, sequence);
    const uint32_t current_generation = LOAD(generation);
    STORE(guard, before + 2u);
    h2_bk_lease_writer_unlock();

    /* The hint may fail when the SDK IPC event buffer is busy. The fixed
     * snapshot remains authoritative and can be read through paired RPC. */
    h2_bk_wifi_lease_wake_t wake = {
        .magic = H2_BK_WIFI_LEASE_WAKE_MAGIC,
        .version = H2_BK_WIFI_LEASE_VERSION,
        .generation = current_generation,
        .sequence = sequence,
        .ip4 = ip4,
        .mac_hi = hi,
        .mac_lo = lo,
        .xid = xid,
    };
    if (cif_send_customer_event((uint8_t *)&wake, (uint16_t)sizeof(wake)) != 0)
        (void)__atomic_add_fetch(&wake_failures, 1u, __ATOMIC_SEQ_CST);
    return sequence;
}

void h2_bk_wifi_lease_snapshot(h2_bk_wifi_lease_snapshot_t *out) {
    if (out == NULL) return;
    *out = (h2_bk_wifi_lease_snapshot_t){0};
    if (LOAD(ready) == 0u) return;
    for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
        uint32_t before = LOAD(guard);
        if (before & 1u) continue;
        uint32_t n = LOAD(count);
        if (n > H2_BK_WIFI_LEASE_CAPACITY) return;
        out->generation = LOAD(generation);
        out->last_sequence = LOAD(last_sequence);
        out->count = n;
        for (uint32_t i = 0u; i < n; ++i) {
            out->records[i].sequence = LOAD(records[i].sequence);
            out->records[i].ip4 = LOAD(records[i].ip4);
            out->records[i].mac_hi = LOAD(records[i].mac_hi);
            out->records[i].mac_lo = LOAD(records[i].mac_lo);
            out->records[i].xid = LOAD(records[i].xid);
        }
        uint32_t after = LOAD(guard);
        if (before == after && (after & 1u) == 0u) {
            out->version = H2_BK_WIFI_LEASE_VERSION;
            out->wake_failures = LOAD(wake_failures);
            return;
        }
    }
    *out = (h2_bk_wifi_lease_snapshot_t){0};
}

#undef LOAD
#undef STORE
