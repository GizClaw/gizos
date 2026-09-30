#include "h2_bk_wifi_lease.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned wake_count, fail_wake;
static h2_bk_wifi_lease_wake_t latest_wake;

int cif_send_customer_event(uint8_t *data, uint16_t len) {
    assert(data != NULL && len == sizeof(latest_wake));
    memcpy(&latest_wake, data, len);
    assert(latest_wake.magic == H2_BK_WIFI_LEASE_WAKE_MAGIC);
    assert(latest_wake.version == H2_BK_WIFI_LEASE_VERSION);
    h2_bk_wifi_lease_snapshot_t snapshot;
    h2_bk_wifi_lease_snapshot(&snapshot);
    if (snapshot.version != 0u) {
        assert(snapshot.version == H2_BK_WIFI_LEASE_VERSION);
        assert(snapshot.generation == latest_wake.generation);
    } else {
        assert(snapshot.count == 0u);
    }
    ++wake_count;
    return fail_wake ? -1 : 0;
}

static void verify_records(void) {
    h2_bk_wifi_lease_snapshot_t snapshot;
    h2_bk_wifi_lease_snapshot(&snapshot);
    if (snapshot.version == 0u) {
        /* A bounded read can report busy during a concurrent ACK write. */
        assert(snapshot.count == 0u);
        return;
    }
    assert(snapshot.version == H2_BK_WIFI_LEASE_VERSION);
    assert(snapshot.count <= H2_BK_WIFI_LEASE_CAPACITY);
    for (uint32_t i = 0u; i < snapshot.count; ++i) {
        const h2_bk_wifi_lease_record_t *r = &snapshot.records[i];
        assert(r->sequence != 0u && r->ip4 != 0u);
        for (uint32_t j = 0u; j < i; ++j)
            assert(r->mac_hi != snapshot.records[j].mac_hi ||
                   r->mac_lo != snapshot.records[j].mac_lo);
    }
}

static void test_lifecycle(void) {
    const uint8_t mac[6] = {0x30, 0xed, 0xa0, 0xae, 0x0f, 0x84};
    const uint8_t bad[6] = {0xff, 0, 0, 0, 0, 0};
    h2_bk_wifi_lease_snapshot_t snapshot;
    h2_bk_wifi_lease_snapshot(&snapshot);
    assert(snapshot.version == 0u && snapshot.generation == 0u && snapshot.count == 0u);
    assert(h2_bk_wifi_lease_accept(mac, 0xc0a8bc64u, 1u) == 0u);
    h2_bk_wifi_lease_reset();
    h2_bk_wifi_lease_snapshot(&snapshot);
    assert(snapshot.generation == 1u && snapshot.count == 0u);
    assert(h2_bk_wifi_lease_accept(NULL, 0xc0a8bc64u, 1u) == 0u);
    assert(h2_bk_wifi_lease_accept(bad, 0xc0a8bc64u, 1u) == 0u);
    assert(h2_bk_wifi_lease_accept(mac, 0u, 1u) == 0u);
    assert(h2_bk_wifi_lease_accept(mac, 0xc0a8bc64u, 0u) == 1u);
    assert(h2_bk_wifi_lease_accept(mac, 0xc0a8bc64u, 0u) == 1u);
    assert(wake_count == 2u && latest_wake.xid == 0u && latest_wake.sequence == 1u);
    assert(h2_bk_wifi_lease_accept(mac, 0xc0a8bc64u, 2u) == 2u);
    h2_bk_wifi_lease_snapshot(&snapshot);
    assert(snapshot.count == 1u && snapshot.records[0].xid == 2u);
    assert(snapshot.records[0].ip4 == 0xc0a8bc64u);
    assert(snapshot.records[0].mac_hi == 0x30edu);
    assert(snapshot.records[0].mac_lo == 0xa0ae0f84u);
    h2_bk_wifi_lease_forget(mac);
    h2_bk_wifi_lease_snapshot(&snapshot);
    assert(snapshot.count == 0u && snapshot.last_sequence == 2u);
    assert(h2_bk_wifi_lease_accept(mac, 0xc0a8bc64u, 2u) == 3u);

    h2_bk_wifi_lease_reset();
    h2_bk_wifi_lease_snapshot(&snapshot);
    assert(snapshot.generation == 2u && snapshot.count == 0u);
    assert(h2_bk_wifi_lease_accept(mac, 0xc0a8bc64u, 2u) == 4u);
    assert(latest_wake.generation == 2u && latest_wake.sequence == 4u);
    fail_wake = 1u;
    assert(h2_bk_wifi_lease_accept(mac, 0xc0a8bc64u, 3u) == 5u);
    h2_bk_wifi_lease_snapshot(&snapshot);
    assert(snapshot.wake_failures == 1u && snapshot.records[0].sequence == 5u);
    fail_wake = 0u;

    h2_bk_wifi_lease_reset();
    for (uint32_t i = 0u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i) {
        uint8_t m[6] = {0x02, 0, 0, 0, 0, (uint8_t)(i + 1u)};
        assert(h2_bk_wifi_lease_accept(m, 0xc0a8bc65u + i, i + 1u) != 0u);
    }
    const uint8_t ninth[6] = {0x02, 0, 0, 0, 1, 0};
    assert(h2_bk_wifi_lease_accept(ninth, 0xc0a8bc80u, 9u) == 0u);
    verify_records();
}

static unsigned active_writer = 1u;
static void *writer(void *arg) {
    (void)arg;
    const uint8_t mac[6] = {0x30, 0xed, 0xa0, 0xae, 0x0f, 0x84};
    for (uint32_t i = 1u; i <= 20000u; ++i)
        assert(h2_bk_wifi_lease_accept(mac, 0x0a000000u + i, i) != 0u);
    __atomic_store_n(&active_writer, 0u, __ATOMIC_RELEASE);
    return NULL;
}
static void *reader(void *arg) {
    (void)arg;
    while (__atomic_load_n(&active_writer, __ATOMIC_ACQUIRE)) verify_records();
    for (unsigned i = 0u; i < 100u; ++i) verify_records();
    return NULL;
}
static void *forgetter(void *arg) {
    (void)arg;
    const uint8_t mac[6] = {0x30, 0xed, 0xa0, 0xae, 0x0f, 0x84};
    for (unsigned i = 0u; i < 10000u; ++i) h2_bk_wifi_lease_forget(mac);
    return NULL;
}

int main(void) {
    test_lifecycle();
    h2_bk_wifi_lease_reset();
    pthread_t producer, consumers[2], remover;
    assert(pthread_create(&consumers[0], NULL, reader, NULL) == 0);
    assert(pthread_create(&consumers[1], NULL, reader, NULL) == 0);
    assert(pthread_create(&producer, NULL, writer, NULL) == 0);
    assert(pthread_create(&remover, NULL, forgetter, NULL) == 0);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(remover, NULL) == 0);
    assert(pthread_join(consumers[0], NULL) == 0);
    assert(pthread_join(consumers[1], NULL) == 0);
    verify_records();
    puts("BK_WIFI_ACK_LEASE_GENERATION_AND_CONCURRENT_SNAPSHOT PASS");
    return 0;
}
